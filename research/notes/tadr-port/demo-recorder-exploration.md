# Demo recorder — technical exploration

## Status and evidence boundary

**Source/disassembly investigation and executable replay experiments, 2026-09-28.** The product contract and feature progress live
in [Demo recorder](demo-recorder.md). This page holds technical findings and the experiments
needed to settle the open architecture decisions. A temporary transport probe has captured a
live two-player battle, and the captured events have been decoded, compared between peers, and
benchmarked in independent compressed blocks. **Solo engine replay (§6a), direct unit-scene playback (§6b), remote-perspective comparisons (§6c), and wider same-level snapshot fidelity (§6d) have run; complete-world seeking has not.**
Preparation and checkpoint policy remain undecided; §6 distinguishes measured container results
from the missing end-to-end evidence.

Sources inspected: TADR's gitignored source checkout at `dcff5dd`, Impure's worktree at
`2430903`, and `pristine/TotalA.exe.pristine` in the main checkout. **SRC** means directly read
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

These remain candidate production boundaries; §6 reports the temporary transport observation. The temporary capture observer is not part of the production patch. Detailed verified addresses are retained
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

## 6. Executable transport and container experiments

**MEASURED 2026-09-28 — reference setup:** Ryzen 9 7950X, x86-64 Linux, Wine 9.0,
retail TA 3.1, Impure `2430903` plus a temporary observation-only transport probe, normal
`--defaults` rendering, 1024×768, native DirectPlay over loopback. Two isolated `tacli`
instances played **Two Continents**. `limits-mp-west` and `limits-mp-east` each added 450
artillery units without removing the two commanders. A subsequent fixture added seven
unfinished units. This is **one two-player stock-content run**, not ten-player, AI, mod,
long-match, loss-injection, or recorder-on/off performance coverage.

The observer byte-matched the three wrapper entries in §3. It copied send arguments while
the caller owned them; on successful receive it copied no more than the caller's original
buffer capacity. Return bookkeeping was per-thread. It wrote bounded records to a 128 MiB
mapped research file and published each record's length after its payload. No engine buffer
pointer survived the callback. This temporary mapping is **not the proposed production queue
or durable writer**. The send observer records attempts, not successful-send acknowledgments.

The probe is removed from the worktree, its normal DLL rebuilt, and both test games stopped
and their instances removed. Compact captures and the exact probe are retained locally under
`_local/demo-recorder-exploration/` in the main checkout. No raw capture or research DLL is
tracked. The offline decoder, container experiment, tests, and numeric results are under
`research/experiments/demo-recorder/`.

### Transport coverage and the actual Pascal decoder

The host trace contains **1,821 records / 287,360 bytes** including the probe header and record
framing; the joiner has **1,795 / 284,400 bytes**. Neither reported capacity drops. Payloads above the probe cap were not recorded; this
experiment does not establish oversized-packet coverage. The largest
observed transport payload was **1,066 bytes**, below the probe's 65,536-byte payload cap.
The traces span 190.195 and 176.419 seconds respectively, including lobby and paused time.
Those full-trace durations must not be used as active-battle bandwidth denominators.

**Source correction:** `Recorder/tplayx.dpr:16` and `idplay.pas:425` import
**`src/packet_old.pas`**. The similarly named `Recorder/packets/packet.pas` is not the decoder
selected by this recorder project. The active constructor decrypts **before** decompressing.
The research decoder follows `packet_old.pas:359–508`, checks its checksum, bounds every
compression reference, then splits using explicit lengths. All non-system transport records
in both captures decoded and split without errors. DirectPlay system messages were counted
separately: their payloads can contain process-local pointers and cannot be persisted as
portable replay events by dumping their bytes.

For each direction, compare the ordered payload list at the sender with the corresponding
list received by the other peer; do not sort or deduplicate these lists:

| Direction | Creates `0x4A` | Damage `0x4B` | Deaths `0x4C` | Comparison |
|---|---:|---:|---:|---|
| Host → joiner | 458 | 1,720 | 278 | Every payload byte and list order equal |
| Joiner → host | 451 | 1,372 | 262 | Every payload byte and list order equal |

The creates include two commanders, the 900-unit burst, and the seven later fixture units.
This demonstrates coverage for **these paths and this workload**, not all possible targeted
messages, all four Impure tags, successful playback, or durable crash recovery. The probe saw
one additional transport thread on each peer; production capture cannot assume one producer.

### Construction-state result and a fixture trap

All **902 initial creates** carried valid state with build fraction remaining **0**. The seven
later creates carried remaining fractions **0.95, 0.75, 0.50, 0.25, 0.05, 0.60, 0.40** with
HP **16, 81, 163, 244, 309, 1076, 595**. Their receive payloads matched exactly.

After both peers paused, a bounded read of the receiver's unit array confirmed the fraction
and HP bytes for six of the seven. **The lab at slot 79 was already at fraction 0**, whereas
the owner's roster still reported 0.60. The trace includes a following stock `0x12` record
`12 4F 00 4F 00`, sent by the owner and received by its peer. Its presence is consistent with
the documented B8 rule that completion remains stock. The causal role of that message is an
**inference**, not a newly traced execution of its receiver.

This fixture creates a completed unit and then changes its fraction; it does **not** model
all steps of real construction. Do not label the seven-unit fixture a clean construction
regression pass or conclude that genuine construction is broken. The new observation is a
specific synthetic-fixture disagreement to resolve when establishing the playback oracle.
A replay must preserve subsequent completion events as well as the initial create state.

### Compression, parsing, and prefix recovery

Use the host trace interval **80,316,576 ≤ probe milliseconds < 80,414,000**: 97.424 seconds,
including the 900-unit burst and combat, excluding lobby, the unfinished fixture, and pause.
It contains **14,544 logical messages**, with **469,997 payload bytes**. The deliberately simple
experimental framing adds 16 bytes per event: **702,701 bytes** before compression. Messages
are wall-time sorted only for this codec experiment; this is **not** a proposed simulation
scheduler or proof of cross-thread causal ordering.

Each independently compressed block has a 32-byte header with stored/raw lengths, time range,
event count, raw-data CRC, and header CRC. The separately calculated index costs 12 bytes per
block. It is a *reference experiment*, not a committed replay schema. No snapshots, player
presentation contributions, final metadata, or asset data are included.

| Block interval, Zstandard level 3 | Blocks | Compressed blocks + estimated index | Whole sample compression, median | One block decompress + parse, p99 |
|---|---:|---:|---:|---:|
| 100 ms | 889 | 191,672 B | 3.137 ms | 0.079 ms |
| 1 s | 98 | **112,070 B** | **1.304 ms** | **0.547 ms** |
| 5 s | 20 | 102,862 B | 0.963 ms | 1.097 ms |

These are **offline, warm-memory Python/Zstandard 0.25.0 measurements**, ten passes on the
reference setup. They include Python event parsing on read, but no disk read, engine message
application, world reconstruction, renderer rebuild, GPU upload, or frame presentation. They
are **not seek times, first-open times, live recording overhead, or a total replay-size claim**.
Do not extrapolate this short, declining battle to an average match-hour.

Level 1 at one second used 114,841 B including the index; level 6 used 106,371 B. On this trace,
level 6 saved about 5% over level 3 but took 3.402 ms rather than 1.304 ms to compress the sample.
**Recommendation:** start the prototype with independently compressed blocks around one second
and a maximum raw-byte cap, using a fast Zstandard setting. This is an implementation starting
point, not a frozen format or checkpoint interval. Compression dictionaries are unnecessary
for this first experiment; measure before adding distribution/versioning complexity.

The seven research tests pass. They cover binary `0x05` preservation, bad packet lengths and
compression references, checksum rejection, oversized-block refusal, event-count/length bounds,
every byte truncation of a two-block synthetic stream, and single-bit corruption that must
either be rejected or decode to the unchanged events. Every measured block also round-trips
its exact event tuples. This demonstrates the **offline prefix-recovery algorithm**. It does
not test process crashes, OS write ordering, `FlushFileBuffers`, disk-full behavior, or the
playability of a recovered prefix.

The probe's timed record-fill region measured p50 **0.4 µs**, p99 **16.8 / 16.9 µs**, maximum
**45.7 / 62.8 µs** on host/joiner. It excludes the detour, TLS setup, failed receive polling,
reservation, publication, and final timing read. Page faults and competing work can affect it.
It therefore cannot be reported as recorder overhead or substituted for a proper off/on test.

## 6a. First solo engine playback

**MEASURED 2026-09-28, same reference setup and captured two-player match as §6.**
A research-only native DirectPlay host (`dplay_bridge.c`, driven by `replay_host.py`) creates
two recorded-player identities, negotiates the roster/map and all 278 unit types, then sends
one participant's captured gameplay messages to a third, spectator-only Impure instance.
No Pascal recorder/server binary is loaded. The prototype borrows the Pascal server handshake;
it is not an integrated replay UI or a final playback backend.

The viewer needs stable recorded unit-block ordering independently of newly assigned network
IDs. An isolated, byte-checked probe changes the allocation comparisons to use immutable
recorded-player ranks. Actual transport IDs remain unchanged; the driver also translates the
killer ID in Impure's carried-death envelope. The [engine map](../exe-reverse-engineering.md#replay-player-names-and-unit-block-ordering-disassembled-measured-2026-09-28)
records the four comparison sites and the invariant the sort requires. This probe is not a
production patch; failure to initialize it must prevent treating a run as valid.

### Baseline result

- All **902 initial units** appeared, including the rapid 900-unit scenario burst, with the
  two recorded ranges starting at slots **1 and 1501**; the spectator's range starts at 3001.
- The driver sent **16,095 logical gameplay events**. Measured from the viewer's first outgoing
  unit-update packet, feeding the stream took **123.692 seconds** at nominal 1× pacing.
  This excludes lobby setup and loading. Launch message to first viewer update was **1.830 s**.
- At the recorded pause, the viewer contained **369 units**: exactly the same surviving slots,
  type names, and integer map X/Y positions as both original peers' final rosters.
- Every unit's integer height matched its **original receiving peer**. Relative to its controlling
  peer, 70 host-owned and 76 joiner-owned units differ in height. This demonstrates agreement
  with the observed network representation, not exact reproduction of all owner-local state.
- All seven fraction/HP samples from §6 match the original receiving peer **byte for byte**,
  including the fixture lab's subsequent completion. The roster reports zero construction
  fraction for the finished burst units; an inspected rendered battle frame shows finished
  units without construction glow. Genuine construction and seek restoration remain untested.
- The final viewer tick was **3709**, versus **3674/3683** in the original paused peers. The
  prototype starts wall-time pacing at the viewer's first update and does not align source
  simulation/application clocks. This is an explicit unresolved timing difference, not a
  fidelity pass for animation, projectile phase, fog, scripts, or time-dependent statistics.

The driver currently uses two human players, wall-time scheduling, native loopback DirectPlay,
and a hand-driven spectator lobby. It counts unit acknowledgements without implementing the
final content-fingerprint contract. Completion of this sample establishes an executable
packet-playback baseline and exact checks for the fields above. It does **not** establish
full-state equivalence, pause/speed UX, compatibility, crash recovery, or a seek result.

### Accelerated catch-up and stock-save probe

The same stream was fed at a requested 100× rate. An initial bridge implementation drained
one command per millisecond loop and took **16.557 s** to submit the stream. Draining up to
128 commands per iteration removed that harness bottleneck: the second run submitted all
16,095 events in **1.240 s**. The engine reached its recorded pause at tick **51**; the earlier
run paused at tick **536**. The driver's submission timer does not include the engine's last
queued receive, and the query loop was not synchronized precisely enough to report a
request-to-correct-frame percentile.

Both accelerated runs ended with the same **369 surviving slots, types, map coordinates and
heights** as the 1× baseline. All **369 six-byte HP/construction-fraction samples** also matched
byte for byte. The revised ordering probe worked with a viewer ID below both recorded IDs
in the first accelerated run (85455649 versus 85455651/85455652).

These results demonstrate rapid packet catch-up for this sample's final unit state. They do
not prove simulation-clock fidelity: 51 ticks cannot reproduce the 1× run's 3709 ticks of
local simulation, and no equality check covers projectiles, scripts, fog, effects, or wrecks.
Nor can 1.240 seconds for two minutes of events predict a five-to-forty-five-minute jump.
Do not use it to approve the subsecond seek target or a preparation/cache policy.

The isolated viewer also tested the stock save/load path with the multiplayer UI restriction
removed. A 369-unit spectator save produced **392,041 bytes**. Stock load rejected it with
“Invalid savegame file”: the loader explicitly accepts game types 1 and 2. A further probe
admitted exactly type 3. A fresh save from the batched run was **399,676 bytes**, but its load
crashed after level teardown, before a restored world was available. Disassembly and the crash
stack identify `0x46CA60` reading the absent restriction/sync object at `main+0x2A30`, then
calling `0x46E160`, which dereferences `this+0x64` at `0x46E167` with `this == NULL`.

**Conclusion: stock save plus UI patches is not a usable checkpoint backend.** It would need
an explicit session/content/restriction-object lifetime and restore inventory, plus Impure's
side state. Skipping the null dereference would not establish those invariants. No successful
restore, checkpoint-creation timing, or restore-latency result is claimed. The file sizes are
one diagnostic save each; they are not estimates for a complete replay checkpoint.

## 6b. Direct scene playback and portable unit poses

**MEASURED 2026-09-28**, reference setup: Ryzen 9 7950X, Wine 9.0, retail TA 3.1,
Impure `a044167` plus temporary probes, 1024×768, Two Continents `200v200` scenario.
This is a renderer-facing alternative to restoring the simulation, not an engine checkpoint.
`research/experiments/demo-recorder/scene-results.json` retains the measurements;
`scene_blocks.py` reproduces the block-compression comparison on local captures.

### Same-level restore and camera movement

A temporary publisher change captured **every live unit's model pieces**, including units
outside the camera reach. A render-thread probe copied validated published packets into three
owned slots, invalidating them at level end or a changed level generation. Selecting a slot
kept camera commands live and disabled pose interpolation from the live world. The existing
level/template reclamation fence still supplied the template lifetime. No engine simulation
state was restored, and these raw packets are not portable files.

| Snapshot | Tick | Units | Pieces including wreck | Wrecks | Raw bytes | Allocation + copy |
|---|---:|---:|---:|---:|---:|---:|
| Early formation | 33 | 401 | 6,116 | 1 | 293,752 | 100.7 µs |
| Later battle | 1,181 | 354 | 5,432 | 39 | 309,224 | 99.8 µs |

Both reported no truncation. The first snapshot contains all **6,115 unit pieces** plus one
wreck piece. Returning to it after the later battle restored the old formation. A world crop
of 498,176 pixels differed from its earlier image by 73 pixels, all at the clipped right edge;
its difference from the later battle was 192,389 pixels. The 73-pixel cause is not established,
so this comparison is not bit exact. Moving the camera while holding the early snapshot
revealed the captured off-screen formation. This verifies unit-pose coverage in that view,
not historical terrain, effects or fog everywhere on the map.

With Vsync on, command-file-to-selection acknowledgements were mostly about one second.
The slow presentation cadence existed before the snapshot selection; no cause was established.
Turning Vsync off through the isolated instance's render-options UI reduced 20 selection
acknowledgements to **1.22–19.29 ms**. This is selection latency, not GPU presentation latency.

A separate presented-image check alternated the two snapshots ten times, with Vsync off.
Three consecutive reference images per destination were identical in the checked crop;
the destinations differed by 116,676 pixels. Each first window grab after the command matched
the destination exactly there. **Command write through completed capture took 169.6–184.6 ms**,
an upper bound including the screenshot process and readback. The crop was x=128..999,
y=180..735 with the live pause banner excluded. The render-options panel remained open at the
upper right in both references; the changed formation was visible outside it. This proves
visible same-level scene switching in this fixture, not a cold file seek or the 5→45-minute
requirement. It also leaves Vsync-mode coverage open.

### Fresh-process unit-pose relocation

A second temporary reader imported just the early snapshot's **401 units and 6,115 poses**
from a 186,876-byte local file. That file zeroed both runtime keys (`unit.o3_key` and
`piece.node`), stored piece indices instead of packet offsets, and retained numeric type/model
IDs and names. On a fresh process and freshly loaded matching scenario, the reader matched
live templates by type/model, name, piece count and base piece, then supplied the **new
session's** template pointers. Missing templates and invalid table ranges were refused.
The existing level fence covered those pointers; the old process's addresses were never used.

All **59 template-piece addresses across the four unit types** differed between processes.
The old formation nevertheless rendered identically over a checked **237,665-pixel** area:
x=128..849, y=180..549, excluding the pause banner and the old render-options panel.
The full world crop did not match (50,208 differing pixels): the new session retained its own
other layers, including visible fire, and the old reference retained the options panel.
This is a positive portability result for unit poses, **not a complete world restore**.
The probe depends on those types already having live template representatives; shipping
playback needs content-table resolution even for a type with no live units. Its fixed compiler
layout and missing content manifest are also research limitations, not a proposed file schema.

### Independent-block storage measurements

A moving sample captured 100 packets at nominal 10 Hz over 9.902 seconds, ticks 5,979–6,276,
with 150 surviving units and no packet truncation. This is a short late-battle workload,
not a 400-unit fight or a representative match-hour. A prior attempted 10 Hz collection under
Vsync on actually sampled about 1 Hz; a paused control advanced only three ticks. Neither is
substituted for the moving sample below.

Zstandard level 3, independent blocks, 25 timing repetitions, exact byte round trips:

| Captured data | Frames per block | Compressed bytes / 100 frames | Median encode / block | Median decode / block |
|---|---:|---:|---:|---:|
| Whole renderer packet | 1 | 3,333,509 | 0.231 ms | 0.081 ms |
| Whole renderer packet | 10 (nominal 1 s) | 834,653 | 0.958 ms | 0.265 ms |
| Whole renderer packet | 100 (nominal 10 s) | 575,819 | 8.278 ms | 2.429 ms |
| Units + pieces, runtime keys zeroed | 1 | 601,191 | 0.043 ms | 0.018 ms |
| Units + pieces, runtime keys zeroed | 10 (nominal 1 s) | 177,372 | 0.206 ms | 0.069 ms |
| Units + pieces, runtime keys zeroed | 100 (nominal 10 s) | 133,596 | 1.833 ms | 0.581 ms |

For one-second blocks, maximum observed decode was **0.617 ms** for whole packets and
**0.0855 ms** for the unit/piece projection. The projection is a compression estimate, not the
fresh-process import format: it still contains original packet-relative piece offsets and
other renderer-only fields. Both comparisons exclude disk, format framing/index overhead,
asset loading, rendering and complete-state validation.

At the nominal 10 Hz rate, the one-second-block byte counts extrapolate to **300.5 MB/hour**
for whole packets or **63.9 MB/hour** for the unit/piece projection (decimal MB). These are
arithmetic extrapolations of this short sample, not measured hour-long replay sizes. The latter
omits effects, world changes, visibility, communications, statistics and presentation data.
Long blocks improve compression but are unnecessary for cheap codec-level random access here.
Recording only changes, separating static type data and quantizing/interpolating continuous
poses remain space-saving candidates; their fidelity and rate are not established by this run.

### What this settles, and what it does not

Direct unit-state playback can restore the old formation, move the camera, and resolve poses
against newly loaded assets without restoring COB execution or running the historical battle.
That makes watch-only state playback a credible seek architecture. The next comparison should
exercise that path alongside deliberate simulation checkpoints rather than assume a stock-save
shortcut exists. **No preparation/cache policy is selected by these partial results.**

The missing capture inventory remains substantial:

- In this first experiment only unit pose coverage was widened. §6d subsequently widens
  feature/wreck/effects reach and tests two views, finding additional camera-dependent caches.
  The production publisher remains camera-limited; the map-feature seed is not dynamic history.
- Fog packets cover the local camera reach. Other players' current sight/radar/exploration,
  alliance sharing and cloaked contacts need authoritative capture or verified reconstruction.
- Effect models/sprites and renderer caches have their own asset references and coverage rules.
  Restoring unit poses says nothing about effect continuity, historical audio or terrain changes.
- Selection/inspection, resources, statistics, communication and minimap/UI remained live.
  A replay backend must supply their historical values explicitly.
- The exchange is latest-frame-wins and may truncate layers. A recorder must have its own
  loss/completeness contract; silently inheriting renderer drops is unacceptable.
- Pose samples need incarnation-aware identities and interpolation rules. Discrete events
  cannot be recovered from interpolation, and recorded simulation ticks must stay separate
  from the viewer's live engine tick.

The probes were removed, the normal DLL rebuilt, and the owned game stopped after capture.
Raw images, packets, probe source and DLLs remain local under
`_local/demo-recorder-exploration/scene/`; no game assets or raw captures are committed.

### Genuine construction and scene switching

**MEASURED 2026-09-28**, same reference setup and temporary scene probe, a separate Two
Continents skirmish. An ARMCOM received a normal GUI order to build an ARMSOLAR. No scenario
or debugger wrote the solar's build fraction. The applier only supplied the commander; the
engine created the solar and progressed it through construction.

| Captured solar state | GameTime | Fraction remaining | HP | Model pieces | Packet flags |
|---|---:|---:|---:|---:|---:|
| Birth | 4,308 | 1.0 | 0 | 5 | 15 |
| Partially built | 4,429 | 0.5911845 | 134 | 5 | 15 |
| Complete | 4,594 | 0.0 | 326 | 5 | 15 |

The partial and complete packets were selected alternately four times after completion.
Each restored solar matched its original image **exactly over 6,800 pixels** (x=575..659,
y=410..489); the two states themselves differed by 2,724 pixels. The genuine nanoframe
appearance returned for the partial state, and the completed state did not acquire a false
construction glow. Command-to-completed-window-grab times were **146.2–165.4 ms**, again
including capture overhead and with Vsync off. The live pause banner and cursor were outside
that crop. This checks one real structure's same-level renderer-state restoration; it does
not establish a production replay regression suite, mobile factory builds or multiplayer
construction playback. The earlier §6a rapid-spawn engine replay and this scene-state test
exercise different backends and must not be conflated.

`research/experiments/demo-recorder/construction-results.json` retains the states and image
comparisons. Local captures and the input script are under
`_local/demo-recorder-exploration/construction/`. The game was stopped and the ordinary DLL
restored. No persistent construction/recorder patch was added.

## 6c. Remote perspectives require authoritative contributions

**MEASURED 2026-09-28**, two native-DirectPlay peers on the reference setup, retail TA 3.1,
normal Impure DLL from `a044167` engine source (documentation HEAD `9590239`), Wine 9.0,
Two Continents, 1024×768, Vsync off. No new engine instrumentation was used. `tacli peek`
read the paused players' LOS buffers in bounded chunks; player records were matched by DPID,
not local seat number. Both peers call themselves player 0. The LOS dimensions were
**336×400**, 134,400 bytes per player, matching half the feature-grid dimensions on each axis.
The buffers remained inside their existing level lifetime; no load/teardown occurred during reads.

| Paused stage | Host / joiner GameTime | Host-owned grid difference between peers | Joiner-owned grid difference between peers |
|---|---|---:|---:|
| Two stationary commanders | 836 / 838 | 0 cells | 0 cells |
| After both commanders moved | 1,620 / 1,623 | 0 cells | 104 counter cells; **34 zero/nonzero cells** |
| Repeated reads while still paused | 1,620 / 1,623 | 0 cells | Same 104 / 34 cells |

The two rosters agreed on both commanders' world positions and heights after movement:
host-owned slot 501 at (1301,7663), height 84; joiner-owned slot 1 at (9136,5918), height 79.
The four LOS buffers were individually byte-identical between the moved and repeated reads.
Thus waiting while paused did not repair the disagreement. The root cause was not identified;
this experiment establishes that matching unit positions do **not** establish an identical
remote sight grid. The renderer's `tagpu_fogwide.c:fogw_build` uses zero/nonzero counters when
true LOS is active, so the 34-cell subset affects that input. This is not a pixel comparison
under every lobby visibility mode, nor a test of radar, sonar, cloaking or allied sharing.

The resource record is also incomplete remotely. At each peer's `player+0x8C`, its first eight
floats read `(1000,25,0,1000,1,0,1000,1000)` for the local commander and
`(1000,0,0,1000,0,0,1000,1000)` for the remote commander. Using Pascal's
`TPlayerResourcesStruct` field names, production is **25 energy / 1 metal locally, zero
remotely**. After adding 20 locally owned ARMPEEP scouts, the host's first three floats were
approximately `(1000,29,4)`, while its remote copy on the joiner still had zero production and
expense. The current-resource fields alone cannot supply the requested analysis graphs.
Other resource totals, transfers and kill/loss accounting still need individual validation.

**Architecture consequence:** capture each player's effective perspective and statistics at
its controlling peer and include those contributions in every participant's recording. The
remote structures can help diagnostics or reconstruction, but cannot be the sole authority for
recorded perspectives. AI contributions belong to the peer controlling that AI. Record both
source time/sequence and recorder application order; the measured local GameTime counters are
not identical, even during the same pause. Define that clock mapping before fidelity comparisons.

### Compact sight-mask experiment

The local sight predicate needs one bit per cell, not the counter magnitude. Bit-packing
134,400 zero/nonzero values gives **16,800 bytes**. Compressing each independent full mask
with Zstandard level 3 gave **75–78 bytes** for the initial/moved one-commander masks, and
**195 bytes** for the host's 1,067-cell visible mask after the 20 scouts moved. The same
34-cell remote sight disagreement remained in that later sample. These are sparse fixtures:
no full-battle bandwidth rate, worst case or 10-player fan-out measurement is implied.

This supports testing changed tiles/runs plus periodic independent full masks. Preserve every
visibility transition at the chosen authoritative boundary; camera/cursor interpolation is
not a way to invent intervening sight states. A complete perspective also needs explored
terrain, contact/detection state, alliances and the actual LOS mode. The size of a sight mask
alone is not their combined network or storage budget.

`research/experiments/demo-recorder/perspective-results.json` contains the measurements.
The bounded read script, grids, rosters and logs remain local under
`_local/demo-recorder-exploration/perspectives/`. Both owned game instances were stopped.

## 6d. Wider world snapshots and camera-dependent caches

**MEASURED, 2026-09-28 — research-only scene prototype.** The next probe widened the
feature-anchor scan to the entire 672 × 800 cell map and widened the effects gather, retaining
the collectors' existing bounds and truncation reporting. This extends §6b's unit-only reach;
it does not turn renderer packets into a production recording format. The reference setup ran
Two Continents, `200v200`, 1024×768, Vsync off. Evidence is summarized in
`research/experiments/demo-recorder/world-results.json`; original packets, images, probe sources
and DLL remain local research inputs.

Three gaps appeared when comparing a snapshot with the paused live scene at newly visited
camera positions:

- **Cached body turns depend on prior drawing.** In the first early packet, 316 of 398 units
  had different live and cached turn triples. Copying all their pieces retained stale headings.
  The corrected saved copy applies `DrawUnit`'s exact signed, wrapped 16-bit difference test:
  if any axis differs by at least eight angle units, use all three live turns; otherwise keep
  the cached triple. It changes the owned copy, not the engine. This is distinct from reading
  already-composed geometry, which must use the turns that composed it.
- **Feature animation selectors remained live.** An old anchor referencing a GAF wreck-pool
  record or animated feature definition did not freeze its appearance. After advancing the
  battle, the old snapshot lost burning-tree fire: 11,136 changed world pixels in the west view.
  The probe now resolves body/shadow GAF frames on the publishing game thread, bounds the
  definition and wreck indices, and carries those immutable asset references with each anchor.
  Their lifetime is only the current level's existing reclaim fence. Disk playback will need
  stable content handles instead of those pointers.
- **Depth-plane existence depends on prior drawing too.** Correcting headings and animation
  left 2,508 changed pixels around shoreline units in the east view. `fill_unit` records the
  composite's existing depth-plane flag; a never-drawn unit can lack that composite. The
  engine's ordinary builder requests depth when `unit+0x114` bit 0 is set or the construction
  fraction is nonzero. The probe captures that bit and supplies the required depth flag in
  the saved copy. Forced-depth cargo builds have a separate path and remain untested.

In the final run, 302 of 398 early units had different cached/live headings. Merely visiting the
two paused views changed 200 units' existing depth flags; the saved copy supplied depth for
255 units, including ones outside those views. These are camera-cache dependencies, not
simulation changes. The engine map records the allocation and turn-refresh branches.

### Live fidelity and historical restoration

| State | Tick | Units | Model pieces | 3D wrecks | Packet bytes | Copy time |
|---|---:|---:|---:|---:|---:|---:|
| Early | 464 | 398 | 6,071 | 3 | 471,680 | 156.6 µs |
| Late | 1,559 | 303 | 4,709 | 73 | 385,204 | 131.6 µs |

Both packets reported zero truncation. The early gather included 11 projectiles, 72 explosion
records, 18 debris records, 3,998 particles and 29 effect models; the late gather included
15 / 75 / 64 / 429 / 79 respectively. Counts describe captured tables, not proof that every
entry contributed a visible pixel. Copy times exclude the world gather, file IO and rendering.

For **each** camera (`eye=(1900,850)` and `(3100,850)`), the checked world mask contains
489,176 pixels: x=128..1023, y=180..735, excluding x=480..659, y=360..409 for the pause banner.
The higher top cut excludes live attack-message text; HUD, minimap and resources are not part
of this world oracle. Three grabs per state and view gave identical checked pixels.

- Early snapshot versus paused live scene at the same tick: **0 changed pixels in both views**.
  Body-turn/depth correction runs only on saved copies, so the live branch does not share that
  correction. The feature-frame capture is shared by both branches at the same paused tick.
- Restore the early snapshot after advancing to tick 1,559: **0 changed pixels in both views**.
  Burning features, units, wrecks and the visible effects returned with the old world image.
- Positive control, early versus late snapshots: **78,524 west / 191,228 east pixels changed**.
  This was a substantially changed battle, not two copies of an unchanged scene.

This establishes same-level snapshot fidelity in these views. It does **not** establish a
portable full-world stream, interpolation through arbitrary times, full-map pixel coverage,
other players' perspectives, historical audio, terrain mutation, or a 5→45-minute seek.
Out-of-map mirrored feature animation has another consumer and was outside the on-map test.
Aircraft, cargo and mod-specific render paths need their own cases.

### Wider capture storage cost

A separate moving sample took 100 packets at nominal 10 Hz over **9.902 seconds**, ticks
464–765, with 398 down to 378 units and no packet truncation. Zstandard level 3, independent
ten-frame blocks, exact round trips and 25 timing repetitions:

| Data | Raw bytes / 100 frames | Compressed bytes | Median encode / block | Median decode / block | Max decode / block |
|---|---:|---:|---:|---:|---:|
| Widened whole packets | 44,247,268 | 2,545,819 | 2.418 ms | 0.728 ms | 1.346 ms |
| Units + pieces, runtime keys zeroed | 18,032,476 | 708,326 | 0.803 ms | 0.255 ms | 0.302 ms |

At nominal 10 Hz these extrapolate to **916.5 MB/hour** and **255.0 MB/hour** respectively
(decimal MB). These are short-workload extrapolations, not measured hour-long recordings.
The whole packet repeats renderer/UI/static data and retains asset pointers; the unit projection
omits the other layers. Neither is a proposed shipping schema. Compared with §6b's sparse
late-battle sample, the busy battle makes a materially larger file: budget decisions must use
workload ranges. Codec time remains small, but the gather, writer, network contribution, file
opening and complete seek path have not been measured by this benchmark. Delta encoding,
static-data separation and lower-rate continuous pose samples remain candidates to measure.

## 7. Architecture conclusions and remaining decisions

### Capture and identities

**Recommendation:** retain an ordered gameplay event stream as the compact baseline. Port the
verified packet knowledge, not the Pascal server wholesale. Capture at an explicit boundary
shared with existing Impure handling, after held-create state is finalized. A transport tap
is now measured for the paths above; a logical-message tap still needs the coverage comparison
before it replaces that boundary. Keep unknown required gameplay messages intact or reject the
recording/playback version; never silently skip them as optional UI data.

Normalize player identities into a match roster, keeping the original DirectPlay IDs and slot
allocation. Key a unit incarnation by its owner/slot **and birth**, not a recycled slot or the
renderer cache's address. Playback must preserve or translate every embedded player reference,
including the killer DPID in `0x4C`, and rebuild Impure's incarnation tables. AI slots must be
associated with their authoritative controlling peer. A viewer must not consume an eleventh
simulation-player slot in a ten-player game.

For the first solo playback prototype, prefer an **in-process replay transport** behind the
existing DirectPlay boundary. It can supply recorded identities without a real network session.
This still requires a measured startup/lobby/loading state machine and correct COM interface
lifetimes; the current forwarders do not provide it. The Pascal drone/session code supplies
protocol examples, not a reusable in-process implementation. Running a separate native
DirectPlay replay server remains a diagnostic alternative, not the chosen shipping dependency.

Keep sender GameTime, sender event sequence, capture/application order, and monotonic
elapsed time as separate concepts. Disassembly confirms that `0x2C` carries the sender’s
`main+0x38A47` GameTime (`0x48B743..0x48B74E`), which the receiver stores in that
player’s `+0x18` (`0x48B963`). It is not a globally synchronized peer clock. The simulation
loop at `0x495490` may execute multiple iterations before the draw: a latest-frame observer
cannot promise every transition. The [engine map](../exe-reverse-engineering.md#perspective-authority-and-tick-boundary-measured-disassembled-2026-09-28)
records its candidate sampling boundary; no recorder hook is installed there. The two peers paused at ticks **3674 and 3683** in this run:
simultaneous wall-time inspection cannot assume equal tick counters. This does not measure
a fixed clock offset or prove what equal ticks mean across peers. Define the replay
clock and application boundary with the continuous-playback oracle before using them for seek
comparisons. Speed controls must scale that scheduler, not just change wall-time delays while
leaving simulation behavior uncontrolled.

The production recorder needs owned immutable copies, a bounded queue safe for the actual
producer threads, and an asynchronous writer. On exhaustion or IO failure, stop at the last
complete recording boundary and mark incomplete; do not block the game indefinitely or continue
with an unmarked hole. A replay session owns its assets, world, and side tables; cancellation,
seeking, and leaving it need an explicit ownership handoff before another session frees them.

### Seeking: the remaining critical feasibility gate

| Approach | Exploration conclusion | Evidence still needed |
|---|---|---|
| Pascal cursor jump plus round-robin resync | **Reject unchanged.** The source suppresses necessary Impure events and has no world checkpoint. | A different corrected design would have to prove reconstruction, rather than assume later updates repair everything. |
| Stock multiplayer save or raw process dump | **Not a validated checkpoint shortcut.** Engine and Impure state have different lifetimes and serializers. | Full save/restore inventory, identity/side-table restore, loading/thread ownership, then live state equality and latency. |
| Packet replay with deliberate semantic checkpoints | **Keep as the preferred next feasibility experiment** because it builds on compact event capture and engine playback. | Continuous engine playback first; then checkpoint completeness and a bounded amount of advance from checkpoint to target. |
| Direct watch-only state playback | **Prototype validated for unit poses (§6b).** Solo scope permits it; same-level scene switching and fresh-process unit-template relocation work. Complete historical world coverage remains open. | An all-map serialized state representation, portable asset handles, poses/effects, player visibility, inspection state, and measured capture/storage cost. |

A checkpoint inventory includes at least roster/ownership/alliances, live units and incarnations,
HP/build state, transforms and COB execution/poses, orders, transports, projectile and effect
state, feature destruction/wrecks, resources and statistics, terrain changes, fog/radar/exploration,
clocks and randomness, and Impure's state outside the engine heap. An engine-simulation checkpoint
and a watch-only visual checkpoint have different inventories; do not label one as the other.

The current renderer packet cannot be serialized verbatim: `TAGPU_PK_PIECE.node` is a live
per-type pointer, `o3_key` is a runtime cache key, some pieces/effects/anchors are view-limited,
and the publisher can drop/truncate data for rendering. Convert pointers to content handles,
copy all required state at its owning boundary, and make completeness explicit before using
that representation for persistent playback. The fact that the renderer can draw it today
proves none of these portability properties.

**There is no measured complete-world end-to-end seek result yet.** The unit-scene prototype in §6b is narrower: same-level visible switching and fresh-process pose relocation. In particular, this exploration has not
established a safe complete engine checkpoint or selected a watch-only replacement. The user's
subsecond forward-seek requirement and undecided preparation policy remain unchanged. The
container timings in §6 cannot settle either. The next work is complete-state coverage and request-to-correct-world comparison of the executable prototypes, not more file-offset indexing.

### Perspective data and network budget

All peers must contribute data that their normal unit traffic does not convey: camera/zoom,
cursor and click events, local selections and accepted orders, private/team communication,
and authoritative visibility/statistics data: §6c demonstrates disagreement in remote sight and missing remote production values. A local
file-writing opt-out should mean **do not save my local replay**, while participation still
provides data needed for other players' one-file recordings; this is a recommendation to make
explicit in the setting, not a claim that such a setting exists.

Use changed-only sampled camera/cursor records and interpolate only continuous fields. A
camera teleport, selection change, click, command, and unit death is a discrete event. Tag
unit references with incarnation, define cursor coordinates independently of screen resolution,
and preserve the sample's source tick/sequence. A lost transient sample can be superseded by a
later full sample; a lost command/chat/selection delta requires reliable delivery or explicit
repair. Interpolation does not repair missing discrete events.

**Arithmetic budget, not a network measurement:** a 24-byte changed sample plus an 8-byte batch
header at 10 Hz is 320 B/s per origin. With ten peers sending to the other nine, that is
2,880 B/s outgoing per origin and 28,800 B/s summed across all origins **before transport
framing**, and before selections, orders, visibility or chat. At 5 Hz those figures halve.
Batching, quantization and deltas can improve this; sending a full 60 Hz stream is unnecessary.
Measure actual framing, fan-out and bursts with the implemented contribution protocol. Do not
advertise these calculated payload figures as measured wire traffic.

The new contribution envelope must be explicitly versioned and length-bounded. Avoid treating
its bytes as chat or allowing an unknown code to reach a stock splitter. Decide the negotiated
message code/envelope when implementing the capture protocol; all-Impure participants allow a
shared protocol but do not remove the need to check matching capabilities.

### Visibility, communication, and analysis

The local renderer's fog grid is insufficient for arbitrary player perspectives. Store or
reconstruct **explored cells, current sight, radar/sonar contacts, cloak/detection and alliance
sharing** separately. `TAMem/TA_MemoryStructures.pas` names per-player `LOS_MEMORY`, its dimensions
and length, resource totals, kills and losses; those names supplied the field leads. §6c validates the measured LOS dimensions and demonstrates that remote sight and production values are not sufficient authorities. Validate the remaining fields and update cadence before choosing their delta/reconstruction schemes.

Capture in-match chat at the accepted message boundary with its actual audience, and forward
private/team messages explicitly when the recorder is not an original recipient. Assign event
identities so the original and forwarded copy become one event. Start that capture at match
start; do not serialize lobby chat and later rely on a display filter. Pings/drawings use the
same audience and timing rules. Seeking applies the chosen perspective at the destination;
changing visibility is not just changing the camera or hiding the chat panel.

For the statistics implementation, the following definitions are recommended for review:

- Capture resource values/totals from each controlling peer. Keep production, consumption,
  transfers, reclaim and waste distinguishable; snapshot team membership when aggregating.
- Show army metal and energy value separately, avoiding an invented universal conversion
  ratio. Define whether unfinished units contribute invested cost and whether captured units
  count in the receiving army; do not conflate ownership changes with completed builds.
- Record built/lost/kill events and the authoritative counters; reconcile them before claiming
  they reproduce the scoreboard, especially for reclaim, self-destruction and transported death.
- Define APM as accepted player command actions per minute, counting one group order as one
  action. Record selection activity separately; do not count cursor samples as actions or
  invent an effective-APM filter without defining it.

These definitions and the exact speed presets remain implementation/product details, not new
agreements substituted for the feature contract.

### Format, recovery, compatibility, and browser integration

Recommended file structure: immutable versioned match metadata and content manifest; bounded,
independently compressed typed blocks; an appendable time index; and a final results/completion
record. Separate required gameplay/schema versions from optional overlay versions. Index entries
point to independently decodable blocks; a world-checkpoint index additionally names the state
needed to restore, not merely the next event's file offset. Do not persist compiler structs,
pointers, OS handles or an entire static map per checkpoint.

Validate stored/raw lengths, counts, arithmetic, checksums, referenced content and feature versions
before engine application. Incomplete final data permits only the verified prefix. An unknown
required block/schema is incompatible; unknown optional metadata may be skipped within its
validated size. Write completion only after prior blocks and the index meet the chosen durable
ordering; atomic rename by itself does not prove those writes reached storage. Process-crash and
power-loss promises must be tested separately and described precisely.

Use an explicit playback-compatibility identifier plus Impure build and content hashes, including
content load order and effective gameplay configuration. A successful stock unit-sync check is
not a collision-resistant content fingerprint. Retain matching installed content as agreed;
there is no decision here to bundle assets or automatically download old executables.

The main-menu replay browser should read bounded metadata/index data without constructing a
world. Store favorites, bookmarks and display preferences separately from the immutable match
stream; offer the agreed cleanup policy with favorites protected. Loading and seeking need
cancellable progress, and publish the destination frame only after its entire world and chosen
perspective are ready. Expose the same session operations through `tacli` so automation measures
request-to-present time and correctness together.

## Next implementation gate and honest stopping boundary

The source/protocol investigation and transport/container experiments now supply a concrete
prototype direction. **The whole feasibility milestone is not complete:** full-state Impure
playback fidelity, complete restore/seek, remote perspectives, real IO recovery, live off/on
cost and representative scale/content tests have not run. Marking them complete would confuse
a lossless captured event stream with a faithfully playable game.

The first **one-file solo continuous playback of a short two-player match** has run (§6a),
and direct scene switching plus fresh-process unit-pose relocation have run (§6b),
with recorded identities and Impure messages. Establish fixed application-boundary clock/state
comparisons and start/end lifecycle coverage, then implement the smallest explicit checkpoint inventory and
compare both seek directions against that baseline. Only those results can choose checkpoint
frequency, preparation policy and final format. Keep the full watch-only-state alternative if
engine restore cannot satisfy the required invariant and latency.

For that gate, add cases for genuine construction and completion, rapid finished spawning,
slot reuse and delayed hits, transported deaths, commander defeat, a departing participant,
AI control, all ten player slots, whispers/team chat, allied visibility, a long match and mod
content. Report actual opening/preparation/cache/seek/live-cost figures together. Do not replace
the missing experiments with the short codec test or silently relax the agreed seek target.
