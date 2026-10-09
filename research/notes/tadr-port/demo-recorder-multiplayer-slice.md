# Demo recorder multiplayer slice

## Status

Started 2026-10-04. This is the **multiplayer slice** in the [recorder plan](demo-recorder.md#milestones-and-delivery-order), separate from S5's skirmish browser and controls. Stages 1–5 landed 2026-10-06; stage 6 and the player who leaves remain ([*Gaps before the first code landing*](#gaps-before-the-first-code-landing), [*Named residuals*](#owner-authority-materializer)). A recording peer captures an observed native world and its separate authoritative contribution ledger when recording is enabled (`tagpu_recorder.c:mp_record_begin`). The puppet host plays such a file through the [owner-authority materializer](#owner-authority-materializer), seeks it ([*The seek of a multiplayer recording*](#the-seek-of-a-multiplayer-recording)) and takes it over ([*The takeover of a multiplayer recording*](#the-takeover-of-a-multiplayer-recording)). Each peer's camera, cursor, clicks and selection are contributions of their own, run between two peers 2026-10-06 ([*Stage 4*](#stage-4-perspectives-and-communication)). Ten seats are stage 5's: the viewer takes player record 10 on every playback ([the ten-seat note](demo-recorder-ten-seats.md)).

The slice's product scope is a recording file sufficient on its own for playback, with contributions from every Impure peer: each player's effective perspective and authoritative state, communication, then playback and takeover. The [product contract](demo-recorder.md#multiplayer-contributions) requires contributions even from a peer that opts out of saving its own recording. A live multiplayer game has no rewind timeline.

## Two-peer baseline

MEASURED 2026-10-04 on the reference setup, retail content, two native-DirectPlay Wine peers on *Two Continents*, current local `main` build `446337c6`, private display. Both games carried `tagpu_recorder.on`. The host and joiner reached the same live battle with one commander per peer. `main+0x2A44` was `5` on both (network bit set). Neither wrote an `.imprec` file, consistent with the `MODE_SKIRMISH` start and continuation gates in `tagpu_rec_capture`. This is a baseline, not a test of an enabled multiplayer recorder.

After the host sent the network Pause, both peers reported paused. The host read GameTime 1339 and the joiner 1338. For each peer, its local player record (seat 0) at `player+0x8C` began with eight floats `(1000, 25, 0, 1000, 1, 0, 1000, 1000)`; its remote record (seat 1) began `(1000, 0, 0, 1000, 0, 0, 1000, 1000)`. Thus a recording on one peer cannot take the remote player's production and expense from that peer's player image. This repeats the earlier [remote-perspective measurement](demo-recorder-exploration.md#6c-remote-perspectives-require-authoritative-contributions), now against the S4d build. Seat 0 is local on each peer; a cross-peer comparison must match players by their transport ID, not by row number.

The first joiner prefix presented an empty provider list; the DirectPlay probe returned `0x80040154` at `CoCreateInstance(DirectPlay4A)`. Recreating that probe instance after the host launch populated all four providers and completed the two-peer game. No recorder conclusion rests on the failed prefix. The two probe instances and private display were removed after the measurement.

## Source boundary audit

`tagpu_recorder.c:olist_read` walks a unit's main and suborder nodes, and `id_of(RL_T_OWNED)` reads the vtable of a node's owned object. The skirmish lifetime argument at the head of that file applies to type-1 and type-2 owners. It explicitly names the type-3 air proxy that can delete its object independently. Three paths reach `olist_read`: normal `cap_orders`, the rotating `cap_verify`, and `dump_write`. The first two run only after a recording starts; `dump_write` can also run from an armed dump in a network game because `dumps_take` is called regardless of mode. Neither the node-vtable test nor the 4096-node cap proves the owned object remains live: both are tests on values read after following the pointer.

The by-design boundary for the network path is to derive a unit's owner from its `+0x96` pointer **as an ID bounded to the engine's ten player records**, then read that record's controller. Locally controlled owners (types 1 and 2) may use the present list lifetime argument; remote owners (types 3 and 4) need their orders from their controlling peer. This must cover all three callers, including dumps, before any network capture or dump can walk a remote list. Native primary-stream capture skips remote lists before calling the reader. Their authoritative contributions retain their exact source incarnation in the separate ledger; materializing them into a playback world remains unfinished.

## Contribution transport checkpoint

The work branch has a checkpoint, not a multiplayer recorder landing. `olist_read` now refuses a network owner's lists before following either head unless the owner pointer names one of the ten records and its controller is 1 or 2. That one boundary covers normal capture, verification and dumps. A puppet host has no network bit; its restored owned-object pointers are zero, so its list dumps retain their existing path.

`tagpu_recmp.c` carries bounded contributions through its own native DirectPlay application and session on one worker. The game thread copies the fresh engine-start match GUID, validated start host ID, native host address and every active human/hosted-AI primary ID, public key and ownership into `RecMpConfig`. Native ownership must agree with controllers 1/2 versus 3/4, and every local key must equal the process signer's public key. The copied configuration contains no engine pointer or primary-session interface. TA's `0x4C9840` receive wrapper and its source/destination writes remain stock; contribution packets never enter that pump.

The 64-byte version-2 envelope carries byte count, primary origin ID, source tick, sequence, kind, match GUID and stream GUID. The game-to-worker transmit queue and worker-to-game receive queue each have 256 slots of 4096 bytes with one producer and one consumer. Atomic publication orders copied packet bytes before slot reuse. The worker's sixteen native async completion contexts bound outstanding accepted sends. A full transmit queue or a malformed/unauthorized logical stream ends contributions incomplete; a full receive queue, or a packet the game cannot apply, ends only this peer's consumption ([*Channel failure handling*](#channel-failure-handling)). No transport call, worker join or queue lock runs on the game thread.

Each local human and hosted AI signs its primary origin and private identity against the public key already copied from that actual primary native player. A remote binding freezes its private SDK ID, session and stream. Data remains queued until all expected remote actors bind and every pending local identity announcement has been accepted before subsequent data sends. Native private membership changes and newly verified actors request reannouncement; a broadcast to an empty session cannot substitute for a binding. A worker waits out `DPERR_NOSESSIONS` during discovery, and opens again after a lost listener-port race at most ten times ([*Channel failure handling*](#channel-failure-handling)). Retry scheduling establishes no completion fact.

Teardown releases the game reference and publishes stop without joining. A reference keeps the wake handle and copied channel storage alive through a concurrent worker finish. There is at most one channel worker per process, including one still finishing. The worker drains queued local work and native completions before closing its own session on the normal path; abort never claims delivery. When native membership has no private peers left, queued transmissions can retire only after every expected source has supplied its final prefix. A departed unsealed source cannot authorize that retirement. A 15-second stopped-worker drain deadline aborts incomplete; elapsed time proves no receipt, prefix or delivery fact. Normal teardown publishes a final prefix seal for each local actor before stop. A validated seal proves that received source prefix; it does not establish a complete recording. The consumption cut, final-seal receipts and retained-consumer API below now govern normal close. Game capture attaches the file context before the first source publication; native authority application remains open.

Locally controlled human and hosted-AI players contribute their full normalized 520-byte player image and subsequent changed-word deltas with recording off. The recipient maps records by primary ID, independently of their private SDK sender. Source sequences are separate for up to ten origins. The game keeps authoritative caches separately from its observed world. Enabled local recording persists both namespaces without altering engine resources. The recording format is 9 (`REC_FORMAT`), the only one the readers take.

The primary descriptor refuses `DPSESSION_NOMESSAGEID`, which removes native source identity. The private backend requires the native protocol, ordered guaranteed async sends and completion notifications on its own session. Its [Send contract](https://library.thedatadungeon.com/directx7/devdoc/live/directx/dpref_6dvd.content.htm) and bounded contexts establish requested transport support, not terminal recording completeness or a measured bandwidth budget.

### Authoritative player images and frozen seats

`tagpu_recmp_players.c` stores the existing recorder's entire normalized player image: the 0x14B-byte record, zero padding, and the process-lifetime PlayerInfo's 0xB9-byte tail. The existing `replay-layout.tsv` masks and unit-ID substitutions are unchanged. A separate owner-controller value distinguishes a human (1) from a hosted AI (2); the recorder's normalized controller byte remains excluded as before. This replaces the production adapter's earlier 32-byte resource-only samples without adding a recorder exclusion.

The copied `RecMpRoster` freezes primary origin, recorder-local seat and inclusive global unit-slot block for each active actor. Bootstrap requires unique origins, seats and blocks, ten fixed blocks of `each` slots plus sentinel slot 0, and each endpoint bounded by the engine's unit allocation. Block rank follows native IDs, not local player-row order (`0x485657..0x4856C0`, `0x4858A6..0x4858E0`). A contributed image must retain that origin's normalized first/last unit IDs. Remote queue authorization uses the same frozen range, independent of a recycled unit slot's current contents. The optional file context copies the 176-byte roster into its owned storage and requires it as `RT_MP_SEATS` in a mode-3 container header. Native capture creates that context before the first publication or consumption when local recording is enabled.

Source kind 6 carries a 32-byte prefix: origin, owner controller, full-image flag, a 17-byte mask for 130 words, and three zero bytes. The first image selects every word (552-byte body, 616-byte application packet); later deltas select changed words, including the PlayerInfo tail. An unchanged image/controller emits nothing. Encoding does not change the baseline. The game commits it only after the channel accepts the exact source envelope. The recipient commits a complete, bounded delta atomically with its source tick, sequence, stream and application tick. Unknown initial state remains unknown; malformed input preserves the last complete image and provenance. This storage holds values and IDs, with no engine pointers or engine writes.

The player-image fixture uses the actual player cache, source ledger, ordered journal and persisted-file ledger. Its four human/AI origins produce full images and deltas, then final seals. A 3520-byte raw journal reconstructs all four exact images with source tick 31, sequence 2 and application tick 101; C and independent Python checks agree. Roster alias/range/count refusals, every truncated full-image body ending at a guard page, malformed masks/fields/spans, backwards clocks, duplicate sequences, changed streams and unchanged state after refusal pass. This is a contribution-history fixture, not an `.imprec` playback or seek test.

This player-image measurement precedes wire-version-2 source-roster carriage.

MEASURED 2026-10-04 on the reference setup, the frozen-block adapter on actual TA, *Town & Country*, two humans and one AI hosted by each, private display, both copied DLLs matching the same tested build. Match `90E688CB-EF8B-4404-8032-B0B2450C5410` has human origins 625786551/625786549 and AI origins 625786546/625786547. All four first 520-byte images match their recipient's reconstructed CRC, source tick 1 and sequence 1: `E77F48D2`, `882CE2A9`, `C7773A09`, `36AD9697` respectively. The host applies the remote first images at tick 128, the joiner at 23; these are separate application times, not simultaneous worlds. Both bind two remote actors, validate source seals and receive nonempty commander queues through the frozen blocks, with loss/failure zero at tick 151. The departing host closes before the joiner exits, with accepted/completed 93/99, queue/failure zero, cuts 2/2 and four receipts while remote final sources remain 0/2. The joiner remains on `ARMMAIN2.GUI`, then exits with accepted/completed 53/59, queue/failure zero, cuts 2/2 and two unread proof markers. Both owned instances/prefixes and display are removed. No network `.imprec`, bandwidth bound, host-migration continuation or AI-planner replay is established by this run.

Full player images do not establish complete owner authority: owner-only unit/effect and declared Impure state, initial remote queue availability and engine playback integration remain open. The file reader reconstructs the available player/order application history separately, as described below. AI planner state and takeover require their own coverage.

### Authoritative order cache

`tagpu_recmp_orders.c` is engine-free. Its 16-byte chunk prefix carries unit slot, list, owning-peer birth stamp, total nodes, first node and this chunk's nodes. At most 45 normalized 88-byte node images fit a private packet. A 4096-node list takes several packets; an empty list takes one. Chunks of one transaction must have the same origin, stream, tick, slot, list, birth and total, consecutive source sequences and ascending node ranges. Only a complete transaction replaces a committed list. Malformed, missing, duplicate or stale chunks leave the old committed list intact and fail the contribution path.

Memory is bounded at level start: 131072 cached nodes, two list headers per unit slot and ten 4096-node assembly buffers. A replacement proves enough pool room, including the nodes its old list releases, before releasing that old list. There are no subsequent allocations in publication. Pool exhaustion ends contributions incomplete; it never makes the game wait for memory or a sender.

Local capture compares full normalized lists, including their origin and birth, and sends changed queues for type-1/type-2 owners only. The receiver authorizes a slot against its source player's inclusive `+0x67/+0x6B` block, normalized and frozen at bootstrap against the level allocation and fixed block size. A queue may arrive before its stock unit create, so authorization does not depend on the current contents of a recycled unit slot. The existing list reader can copy a remote queue only while its channel is live and the cache's origin and exact birth match the local unit's declared `s_hitStamp` column. A missing or lower-bound stamp still refuses the list; no remote node or owned object is followed.

Each committed queue retains its source stream GUID, source tick, final source sequence and recorder application tick. The source tick belongs to the sending peer; the application tick is this recorder's B2 tick when the final chunk publishes the complete list. Local capture supplies the same tick for both. Partial or refused transactions preserve the old queue's provenance together with its image. The file's separate application ledger supplies global application order; cache metadata alone supplies neither a clock mapping nor a completeness claim.

The logical receive stream discards our own stream and repeated physical deliveries. A new remote origin starts at sequence 1; every subsequent logical frame must advance by one without its source tick going backwards. A same-sequence frame must equal the last frame byte for byte. A source gap or changed stream fails the channel. A subsequent source seal can expose a missing last DATA packet. A missing seal remains an unproved tail; data receipt alone never marks that source complete.

### Source-prefix seals

`tagpu_recmp_stream.c` keeps a ledger for each local producer and each verified remote source. DATA and seals share a contiguous sequence. A seal's packed 24-byte body names the preceding sequence, closed source tick, cumulative DATA application bytes, CRC-32 over every DATA header/body, and a bounded final flag. A received seal must exactly match the ledger. Sequence and byte-total overflow, a changed cumulative checksum, backwards ticks, data for a sealed tick, or anything after a final seal refuse without changing the ledger. Source/match/session authorization precedes this engine-free check; the checksum is an integrity diagnostic, not the authority argument.

The game calls `tagpu_recmp_advance` before B2 capture. Advancing the source clock closes the preceding capture tick; repeated visits to one GameTime do not close it. Changed prefixes receive a seal and unchanged sources receive a watermark after 30 advancing ticks. A successful finalization freezes each local producer and appends a final seal. Capture/order failure instead calls `tagpu_recmp_abort`, so a refused capture cannot emit a successful terminal prefix. A full queue refuses rather than omitting its seal. Cadence and extra traffic remain subject to the measured budget gate; these constants are not a bandwidth proof.

The receive worker validates the prefix before publishing the marker. At the game consumer, `tagpu_recmp_orders_idle` also requires that source's last queue transaction to be complete. `seals` and `ended` in the channel statistics count worker-validated markers and final sources; they do not count durable file records or prove a recorder application cut. `sent`/`bytes` count accepted source packets, including seals; native `completed` includes signed identity reannouncements too.

The controlled stream fixture passes against the actual ledger: a missing last DATA before its valid seal, altered carried data with unchanged counts, malformed sequence/tick/byte/checksum/final fields, exact guard-page spans, a closed tick, empty/final sources, count/sequence overflow, and unchanged state on refusal. The actual channel's `tail` control withholds the last DATA and delivers its correct final seal; the channel refuses incomplete. The queue-cache fixture proves a partial human transaction prevents its seal while an independently completed AI transaction remains idle.

The earlier prefix-only native fixture starts after both primary Close/Release publications, sends sixteen DATA packets, then an ordinary and final prefix for each human/AI source. Both peers receive all twenty source packets and validate both final ledgers (sequence 10, closed tick 2, 640 DATA bytes per origin). The host completes 26 sends / 3792 application bytes and the joiner 22 / 2352; the additional packets are identity reannouncements. Both finish pending/queued/unread/failure zero and final sources 2/2. Primary Close measures 2506/2502 ms in this standalone fixture, without a stock comparison. Test barriers keep both sessions alive until PASS; this does not prove a shipping terminal handshake, a recording cut, ordinary TA exit or file completeness.

MEASURED 2026-10-04 on the reference setup, actual TA on *Town & Country*, two humans and one AI hosted by each, private display. Both copied DLLs match the tested build. Match `CAAAD143-DA21-4E60-AAC2-4E9DFF46D193` has human origins 617134068/617134070, AI origins 617134065/617134064 and private SDK IDs 617285365/617285367. Both bind the two remote actors, receive resources/nonempty queues and validate ordinary source seals. Source-tick-2 queues publish at recipient ticks 11 and 54. Tick-151 host statistics show 47 accepted source packets / 5488 application bytes / 31 logical receives / 15 seals; the joiner shows 35 / 3952 / 61 / 32, with one queued packet. Both report pending/lost/failure zero. These are separate observations, not a network budget measurement.

Both peers then return through their own ordinary surrender/main-menu paths. The host's worker ends with queued/unread/failure zero, having received no remote final seal; the joiner validates both host final prefixes at source tick 206 (human sequence 23 / 1436 DATA bytes; AI sequence 40 / 2948 DATA bytes). Its worker ends queued/failure zero with final sources 2/2 and two unread markers. Thus normal shutdown is verified, but **file completeness is not**: the host lacks the remaining source's final prefix and the joiner's game consumer has already stopped before consuming two markers. Recorder application cuts, receipt acknowledgement and post-teardown writer ownership must address those facts before enabling files.

The first exit test waited for both workers after leaving only the host; the second incorrectly expected the joiner to reach `ENDMSN`. In this four-seat game the joiner remains on `ARMMAIN2.GUI` with its human and AI commanders after host departure. The corrected test inspects that peer's current screen and leaves it through its own menus. Both owned instances/prefixes and the private display are removed. The standalone ledger/channel controls and the actual queue-cache partial-transaction test pass; no multiplayer file or replay gate is claimed.

### Consumption cuts and terminal receipts

`tagpu_recmp_take_at` records the consumed packet's origin, source stream/sequence/tick, the actual recipient B2 tick and a distinct 64-bit consumption ordinal. `end` freezes these per-source points through its atomic stop publication. The worker reads them only after acquiring stop. A subsequent take by the explicitly transferred sole consumer drains proof tails without extending the frozen cut. `tagpu_recmp_cut` returns those immutable points after end; an unconsumed source has sequence/order zero and still needs a received seal. This is packet-consumption metadata: a partial order transaction is not a committed queue, and these final points alone are not the file's semantic application history; the decoder below reconstructs complete queues separately.

Normal close requires a validated source seal covering every frozen consumed sequence while consumption has not ended (once it has, no recording takes the cut, and `tagpu_recmp_cut` refuses it), all local final markers sent or legitimately retired, all outgoing receipts submitted or retired when no verified recipient remains, and zero queued/native-pending sends. Every source that remains active must also acknowledge each local final marker. An already-ended source needs no future local contributions for its perspective, but its prefix still has to cover the recording's own consumption cut. Thus the leaving recorder can finish while other players continue their match; it does not wait for their final match tick. The existing deadline only aborts incomplete.

A receipt is a 48-byte body naming the target primary origin, source stream, final sequence and exact 24-byte seal. Its control envelope has sequence/tick zero and does not advance that sender's DATA ledger. Its sending actor must already have a verified native binding. The target producer compares the whole receipt against the worker's copy of the accepted local final marker; a malformed or altered receipt refuses. Broadcast receipts for another producer have no effect. Duplicate exact receipts count once. Hosted human/AI actor receipt masks are bounded by the frozen ten-actor roster. Native completions include these 112-byte packets, separately counted in `receipts_sent`.

`retain` requires an already-owned channel reference. The game may transfer its sole receive consumer only after `end` publishes the cut and releases the game reference. The retained reference keeps copied packets, cut points and the wake handle alive after worker finish; `finished` publishes the worker's terminal result. The final release frees the channel. No engine pointer, primary interface or engine read enters this lifetime. The optional file context uses this transfer; game capture does not yet create it.

The actual production cut controls pass with event-selected interleavings. Two remote sources remain active; one sends a later DATA packet and seal after the consumer has ended. Both cuts resolve and all four final receipts validate. A retained consumer drains three late packets after worker finish, while its recorded sequence/ticks/ordinal remain unchanged. With one receipt withheld, cuts 2/2 and zero native pending still fail incomplete after the deadline. With one cut proof withheld, receipts 4 and zero pending still fail with cuts 1/2. Altering the final receipt checksum refuses immediately. These controls exercise the production channel and authentication/ledger code at controlled native-backend boundaries; they do not write a file or simulate a native COM completion.

The native production-channel fixture also passes after both primary interfaces close. Each peer receives sixteen DATA packets and four seals and validates four terminal receipts; each sends four receipts. The host completes 30 sends / 4240 application bytes and the joiner 26 / 2800, with pending/queue/unread/failure zero, final sources 2/2 and cuts 2/2. Identity reannouncements account for the remaining sends. Primary Close takes 2518/2519 ms in this standalone run, without a stock comparison. The test still retains its external barrier; the active-source cut control exercises closure without remote final markers. Physical bandwidth, file durability, all owner authority and replay remain open.

MEASURED 2026-10-04 on the reference setup, actual TA on *Town & Country*, two humans and one AI hosted by each, private display, both copied DLLs matching the tested build. Match `409EB149-7C91-457F-A7FA-D1A0A1EC3DEB` has human origins 619677058/619677056, AI origins 619677063/619677062 and private IDs 619698137/619698139. The host returns through ordinary surrender/main-menu; its worker finishes **before the joiner leaves**, with cuts 2/2 and four final receipts despite remote final sources 0/2. It reports 62 accepted source packets / 68 native completions, queued/pending/failure zero and one unread late packet. The joiner is still on `ARMMAIN2.GUI`; after its own menu exit, its worker reports 39 accepted / 45 completed, cuts 2/2, final sources 2/2, four receipts sent, queued/failure zero and two unread markers. Its received host final prefixes close source tick 205 (human sequence 23 / 1436 DATA bytes; AI sequence 39 / 2780 DATA bytes). Thus closure against a still-playing peer is verified; **no file is written**, and the future writer must retain and drain those proof tails without applying them beyond the frozen cut.

The first launch attempt stopped in `mp_lobby.sh` because its `tacli ls` inventory raced the standalone fixture's removal of an owned instance. No game started. Running the tests sequentially removes that fixture race; the subsequent live test above passes and removes both owned game instances/prefixes and its display. These results do not establish cross-address discovery, a full host-migration match, a network budget, all authority, file playback/seek or ten-seat takeover.

### Recording journal

`tagpu_recmp_record.c` provides a separate bounded journal for the world-file writer. One game-thread producer copies both locally published packets and remotely consumed packets, giving them one 64-bit ordinal and the actual producer/consumer application tick. The 16-byte metadata distinguishes production from consumption; the original 64-byte source envelope and payload retain their source clock, sequence and stream unchanged. Advancing a source closes its prior tick, but its locally produced seal receives the current capture tick as journal metadata. A post-end proof-tail take never appends another application record.

Its copied 112-byte roster contains the fresh match, recorder origin and up to ten primary origins/local roles; it contains no key or engine address. The journal uses a 16 MiB SPSC byte ring. Atomic publication follows every copied byte, including a wrap marker; only the writer's read publication gives that room back. A full or malformed journal fails locally without overwriting unread bytes. The channel detaches that failed journal and continues its mandatory contributions to other recorders. An explicit game-thread detach also precedes ending/freeing a journal while the contribution channel remains active. A journal can be claimed for one channel only, before any packet publication/consumption.

The producer's end publication is its last access; a writer frees only after acquiring end and draining the journal, or discarding a failed journal. No worker accesses the channel's game-owned journal pointer. `RecRing` has an optional copied contribution context and `tagpu_recwriter.c` drains it as described below. Native capture transfers that context into the writer before its thread starts. Authoritative-field materialization into the engine and all player views/communication remain open. The reader's separate authority queries rebuild semantic player/order state after seek.

The journal controls pass copied-body and odd-length physical-wrap checks, a bounded overflow, producer-end hand-over and role/match/span/kind refusal. The cut controls attach the actual journal to the production channel: they retain two consumed packets and three local production records at recorder tick 100, then freeze it. Later proof-tail packets drain without adding application records. A full local journal also leaves the contribution channel healthy, so other recorders do not lose that source when the local recording fails.

MEASURED 2026-10-04 on the reference setup, the standalone native production-channel fixture writes each peer's journal after both primary interfaces close: 40 records / 4016 bytes per file, with twenty local productions and twenty remote consumptions covering the four human/hosted-AI origins. The C fixture rechecks each source ledger; an independent Python decoder reads the persisted bytes and checks consecutive application ordinals, nondecreasing application ticks, roles, source sequences, cumulative DATA CRCs and final seals. An altered DATA byte and a truncated tail both fail validation. These fixture journals have no world snapshot or `.imprec` header and are not playable recordings.

A separate producer/writer-thread control copies and validates 15,000 alternating maximum-size and odd-length records across physical wraps. Consumed-batch events bound outstanding bytes; after acquiring producer end and draining, the writer frees the journal and signals an independently owned completion event. This exercises the actual final-access/free ordering without relying on how quickly either thread runs.

### Contribution file container

The recorder writes format 9 and the readers take format 9 alone. `RT_MULTIPLAYER` carries the copied 112-byte roster in a mode-3 header; both writer and readers refuse a network mode without that roster or a roster in another mode. `RB_CONTRIBUTIONS` carries consecutive 16-byte application metadata plus exact source packets, with no ring padding. Its block timestamps are zero: packet metadata retains application/source clocks, and these blocks are not world seek snapshots. After game hand-over, proof-tail records use phase 3, zero application ordinal and zero application tick. They enter the persisted source ledgers without entering the world copy or extending application cuts.

`tagpu_recmp_file.c` owns the attached journal and a retained channel reference. It is created before channel publication/consumption and transferred into `RecRing` before writer start. The game ends that context before its final ring publication. Ending a local save detaches the journal and publishes a distinct local-stop state; the file writer cannot take the channel consumer or stop game contributions in that state. Ending the channel first permits the sole receive consumer to transfer after journal drain. Context end is the producer's final access; the writer acquires it before freeing either the journal or the retained reference. No engine allocation or interface is kept alive by this context.

The writer independently reconstructs bounded per-origin source ledgers from copied records, including stream/match/role checks, consecutive application ordinals and nondecreasing application ticks. An `RT_MP_END` footer contains version/count, the application-record count and ten 40-byte consumption-cut rows. Local and unused rows are zero. Remote rows must equal the cuts reconstructed from consumption records, using the separate remote-consumption ordinal; a local source must have its final seal and every remote source must have a seal covering its application cut. The writer also requires the channel's terminal result. Missing transport proof or failed local storage abandons the file without a complete end block. C open/summary/info and Python segment parsing validate persisted contribution blocks and that footer before reporting a complete network container.

MEASURED 2026-10-04 on the reference setup, `tools/replay/recmp_file_test.py` runs the actual file writer, compressor, path lock and C readers with the production channel at controlled native boundaries. The normal synthetic world file is 3,108 bytes: 21 unit slots, `each=2`, four described seats and their copied origin/block mapping, world ticks 100/101, eighteen application records (eight remote consumptions) and five late proof records. Both readers validate it; the C reader reads both world ticks and seeks back to 100 with the world player image unchanged by proof-tail DATA. Missing cut proof, a missing final receipt, a full journal and a local save stopped while contributions continue each produce an incomplete file whose durable tick-100 world prefix remains readable. Altered DATA, altered application ordinal, a truncated logical tail, an omitted footer and a changed application cut are refused by C open/summary/info and Python, even after rebuilding the outer block checksums.

This test launches no TA game and uses no engine capture. Its world and player/unit/order images are synthetic. It verifies the contribution container, ownership path and the semantic player/unit/order history below, not a faithful game-produced multiplayer recording or network playback. Native capture is verified separately below; the puppet host still refuses network headers.

The mode-3 header now requires a separate packed `RT_MP_SEATS` (tag 28): the validated 176-byte frozen roster, in the same origin order as `RT_MULTIPLAYER`. The file context validates and copies it before journal attachment; modifying the caller's roster afterwards changes nothing. Writer and both readers require its slot/each counts to match the world allocation, distinct recorded seats described by `RT_PLAYER`, and local-versus-remote controller roles to agree with the source roster. The writer additionally requires exact agreement with its owned context. Two multiplayer segment headers with different source/seat mappings cannot share a chain shape. This metadata provides identities and bounds; it does not materialize source state or prove initial authority is available.

The controlled file fixture verifies all five normal/failure lifecycles with that copied mapping. Twenty-three rebuilt-CRC header mutations cover missing/duplicate/short mappings, version/count/slot/each mismatches, unknown/aliased origins/seats/blocks, unaligned/outside/wrong-sized intervals, nonzero unused rows, missing/duplicate/short seat descriptions, wrong ownership and a falsified network mode. C open/summary/info and Python all refuse. The falsified-mode control exposed the file-info path's mode-dependent validation; file info now validates the entire header before choosing its scan. This is bounded file parsing, with no engine read, write or patch.

### Source-local player rows

Player contribution bytes retain the owner's local player-row coordinates. The recorder's frozen roster alone cannot interpret those coordinates when another peer orders its rows differently. `tagpu_recmp_seat_map` accepts two copied, validated rosters with identical origin sets and fixed global unit blocks, independent of actor-array order. It maps active rows by native origin, pairs vacant rows in ascending order and fixes dummy row 10. This is an eleven-row bijection, preserving even inactive vector entries instead of dropping them.

`tagpu_recmp_player_map` maps the five 11-byte vectors at player `+0x108`, `+0x113`, `+0x11E`, `+0x129` and `+0x134`, and self row `+0x146`, in a copied 520-byte normalized image. It first requires the self row and normalized unit-block endpoints to agree with that source origin. Every other byte is retained, including PlayerInfo, start position and scoreboard rank. The destination is published only after validation and can alias the source image. This utility reads no engine memory and writes no engine state.

The player fixture executes 640 roster permutations over all actor counts 1–10, reversing the destination actor-array order. All 3,520 player images return byte-exactly through the inverse map; active identity, vacant/dummy rows, unchanged other fields, in-place mapping and destination canaries pass. Fifteen refusal controls leave the output unchanged. Exact-size copies ending at a guard page and invalid lengths/capacities with an inaccessible source exercise the span bounds. The immutable source-roster cache also refuses wrong spans, versions, kinds, sequence, origin and stream, retains copied values after caller mutation, and refuses duplicate announcements without replacing the map. The existing cache/journal checks still pass in C and independent Python. Python's file history now independently implements roster validation and the same eleven-row permutation.

Source kind 7 carries the owner's copied 176-byte frozen roster at sequence 1, before its first full player image at sequence 2 and before orders. Each origin announces the whole source peer's roster once. Exact origin sets and fixed global block endpoints must agree with the recorder; actor order and local row assignments may differ. The game adapter validates each local image against its frozen source map and each received image against that sender's map. Raw delta baselines stay in source coordinates. Maps belong to the game thread and are freed at failure/level end; the worker still owns only copied packets.

File history owns separate source and application map caches. A proof-only roster validates a proof-only full image but publishes neither mapping nor player authority; both C and Python fixtures check this. Produced/consumed player queries return recorder-coordinate vectors/self row, retaining source stream/tick/sequence and application tick. The wire version is 2, shared by start tokens, primary keys, private identity and envelopes. Experimental wire-version-1 contributions are refused; the 176-byte roster schema and file metadata schema remain version 1. The file format carries observed replica creation; no recorder exclusion changes for it. Unit coordinates are bounded and mapped as described below. Explored-map contributions use origin identity instead of row bits; projectile owner indices and declared Impure player IDs still require their own remapping audit. Native file capture is enabled on request; engine replay application remains unfinished.

### Owner unit records and movers

`tagpu_recmp_units.c` caches the exact 328-byte normalized unit/mover image. A copied source-to-recorder roster bijection maps normalized owner/resource-player IDs at `+0x96`/`+0xEC` and attacker/owner rows at `+0xF4`/`+0xFF`. The frozen origin must own the global slot block, and both owner fields must agree with its source row before mapping. Nullable player IDs are bounded to 0..11 and row bytes to 0..10; zero and dummy ten retain their meanings. Slot zero and its engine-only owner byte 255 are refused. Every other byte is retained. The [engine map](../exe-reverse-engineering.md#player-row-coordinates-across-multiplayer-peers-disassembled-2026-10-04) records the array/create/resource/damage/death writers and their callers, including helpers read but not used.

Wire kind 8 is a bounded batch: a u32 count followed by up to 201 variable-length entries. Each packed 20-byte prefix carries slot, live/full flags, exact birth and an eleven-byte changed-word mask. Full live entries include all 82 image words; a delta requires a complete live baseline of the same birth and cannot change the type without a full image. Absence is an explicit full entry with no image words. Unknown slots stay unknown until a validated entry names them. Source stream/tick/sequence and application tick accompany each published slot. Birth's high bit is refused. Raw source images remain delta baselines; queries return mapped copies.

The entire copied packet, masks, reconstructed images and distinct slots validate before publication. Source roster and cache allocation must agree **before indexing**, including absent slots. No allocation or failing operation lies between validation and the copied-state writes; one game-thread owner supplies ordering. Invalid input leaves every slot unchanged. The isolated Wine fixture passes 640 roster permutations and 464,640 exact inverse unit mappings, sixteen unchanged-output mapping refusals, twenty-seven atomic packet refusals, full/delta/type/birth/death transitions and exact provenance. Guard controls cover the exact span, all 372 truncations of a mixed live/absent packet, a maximum 201-entry batch, mismatched map/cache allocation, the maximum 65,531-slot allocation and final-slot bounds. These are codec/cache controls, not engine application.

The game adapter copies only frozen local type-1/2 human or hosted-AI blocks. A live slot's bounded owner pointer must agree with that actor before following its mover. Birth is read from the existing declared `tagpu_patches.c:s_hitStamp` table, bounded by its own row count; live unknown births refuse capture. A cleared alive bit proves absence even when a never-created slot's raw stamp is `HIT_UNKNOWN`; the absence record then uses birth metadata zero. This does not change, omit or record the raw Impure table, whose complete authority is still outstanding. Accepted packets populate owned caches and the existing isolated contribution channel. No engine field write or byte patch is added.

This carrier covers the unit record and mover. It does not yet carry its COB/model instance, order-owned objects, effects/projectiles, AI planner or complete declared Impure state. It supplies copied authority queries separately from `RecWorld` and engine application. Native file capture retains both namespaces; faithful engine materialization remains unfinished.

### Semantic application authority

`tagpu_recmp_history.c` validates source bodies as well as their envelope ledger. Its frozen roster bounds each order slot by its origin's inclusive unit block. Full player baselines, exact masks and block IDs use the existing player codec; order chunks use the existing complete-transaction cache. A seal cannot close an incomplete source order transaction. Invalid input poisons the history, and no state can be queried afterwards. Writer and C open/summary/info use this validation; Python's independent `History` decoder enforces the same body and transaction bounds. Legacy resource-kind records remain opaque ledger entries and establish no complete player or order authority.

Every history owns two bounded player/unit/order cache groups. Source caches consume all records, including proof tails. Application caches consume only produced/consumed records, preserving the last complete image/list/absence and its source stream/tick/sequence and actual application tick. The proof validator uses the last application tick internally while it verifies later source packets; it never assigns those proof packets a recorded application time or publishes them. Each order cache has 131072 nodes, two list headers per bounded slot and ten 4096-node staging buffers. Unit caches allocate one copied state per frozen bounded slot. No engine pointer enters this storage.

`recr_mp_player`, `recr_mp_unit` and `recr_mp_orders` reconstruct authority at the reader's current world tick. Forward queries consume new application records only; a seek, changed segment or refreshed file index rebuilds from that segment's application prefix. Contribution blocks are read with bounded sizes and checked checksums, using a separate owned payload so querying cannot invalidate a partly read world block. The file cursor is restored after querying. Unknown baselines/lists/slots stay unknown; a known absent unit returns a copied absent state. These APIs return copied values separately from `RecWorld`; they do not merge authority into the world or write the engine. Python `History.at(T)` independently rebuilds the same application prefix.

The controlled 3,108-byte file, with 18 application records (eight remote consumptions) and five proof records, reconstructs four synthetic 520-byte player images at application tick 100, with source ticks 100 for the local human/AI and 8 for the remote human/AI, sequence 2 and the correct source streams. The remote peer describes rows 7/4/0/2 for origins 11/22/55/66 while the recorder uses rows 0/1/2/3; all five vectors, including vacant/dummy entries, and each self row are checked through playback/seek. Local AI slot 3 and remote AI slot 7 carry full unit/mover images at source sequence 3, with exact births 23/17 and source ticks 100/8. Owner/resource-player IDs and owner/attacker rows are checked after mapping. Slots 4/8 are explicitly absent; unreported slot 5 stays unknown. A proof-only delta changes remote unit word `+0x68` from 66 to 67 in source validation, while application playback/seek keeps 66 and its original provenance. Proof-only roster/player/unit packets create no applied authority in either decoder.

The same file retrieves all 88 bytes of a recorded nonempty local secondary list, including after seek. An empty remote list at source tick 8/sequence 4/application tick 100 remains the last complete list when a 46-node replacement begins at application tick 101. Only the proof tail supplies its final node and a changed player image; source validation reconstructs 46 nodes and the changed image while playback/seek still return the previous complete list and player image. Tick 99 and an unreported slot/list remain unknown. All five file lifecycles and the 23 header controls pass. Forty-three additional body mutations rebuild both outer checksums and source-prefix seals; the raw framing/cut ledger accepts each, but C open/summary/info and Python reject malformed source rosters, player/unit fields and masks, live/birth/owner/block/duplicate-slot violations, order ownership/bounds/continuations and a seal over an incomplete transaction. No tolerance or exclusion changes.

The earlier format-5 container checkpoint passed the actual TA `secondary-orders` regression through live recording, puppet playback, in-place restore and fresh-process seek: complete tick-1080 recording, image/event comparisons and three exact round trips at 481/511/541. The mapping checkpoint's later run ends complete at tick 1080 with 1080 iterations; world/radar/event/count comparisons and round trips at 361/421/451 pass, poisoning 26 tables with zero residue and equal before/after dumps. Its images inventory gate fails at two bytes of `main+0x531`, the active GUI heap pointer. Disassembly and full pointer measurements are in the [engine map](../exe-reverse-engineering.md#the-active-gui-pointer-and-the-images-inventory-disassembled-measured-2026-10-04). Those four bytes are the `leave` row `guitop` (the owner's decision, 2026-10-06); `secondary-orders --cover` then passes all 25 checks, the images gate included. The semantic reader also opens, summarizes and inspects that real skirmish file. The harness's 39 logic tests pass after refreshing the coverage map for the new reader functions. These are skirmish reader/regression results, not native multiplayer replay coverage. Engine application, complete unit/effect/Impure authority and game-produced network record/play/seek remain open.

### Owner-only named script starts

The owner-only COB starts use the existing `RK_EV_START_NAMED` engine root and
payload. The retained legacy wire-kind-9 decoder accepts a u32 count and variable entries: an eight-byte
prefix (`bytes`, `before_capture`, reserved zero, exact incarnation token), then
24 fixed payload bytes and a non-NUL name of 1..32 bytes. Unit IDs are normalized
global slots plus one, authorized against the origin's frozen block. Arguments
remain the existing four copied words; `argc` is 0..4 and run-now is boolean.
The source roster/stream, source tick/sequence and application tick validate
before an entire batch publishes. Up to 122 minimum-size calls or 62 maximum-name
calls fit the 4,032-byte body. Incarnation tokens can exceed GameTime when a slot
is created repeatedly within one tick; they are identities, not tick ceilings.

Every locally controlled human and hosted AI contributes independently of
`tagpu_recorder.on`. Production now carries these starts beside replies and
sprays in the unified kind-10 queue described below; the legacy kind-9 cache and
reader remain available. `recmp_named_test.py` exercises the copied legacy cache:
28 unchanged-cache refusal controls, all 94 guarded truncations of a two-call
batch, maximum batches and copied ownership. The queue controls are in
`recmp_roots_test.py`. Latest-batch queries support inspection; the file's ordered
ledger retains every batch and supplies the complete root sequence.

MEASURED 2026-10-05 on the reference setup: native *Two Continents*, two humans and
one hosted AI each, `each=1500`, private display, neither peer
carrying `tagpu_recorder.on`. The stopped-instance setting requested speed 20,
but the in-game heartbeats show native speed 10: this run provides normal-speed
multiplayer evidence. Each peer creates 20 locally owned human units and
ten units for its locally controlled AI through the scenario applier; all 64
initial/created full unit images match source/recipient raw checksums and provenance.
First named batches match exact bytes, including their four argument words:

| Origin / owner | Source tick / sequence | Calls / bytes | CRC | First unit ID / birth / name | Recipient tick |
|---|---|---|---|---|---|
| 640288370, host human | 128 / 179 | 2 / 89 | `59EEE76E` | 1521 / 69 / HitByWeapon | 87 |
| 640288375, host AI | 119 / 136 | 1 / 49 | `190F3149` | 4509 / 69 / TargetCleared | 77 |
| 640288368, join human | 97 / 163 | 2 / 89 | `90A8148C` | 21 / 49 / HitByWeapon | 142 |
| 640288374, join AI | 92 / 116 | 1 / 49 | `69BC56E9` | 3011 / 49 / TargetCleared | 137 |

These first batches have post-B2 phase zero. The independent codec/queue/file
controls exercise both phases; this live observation does not establish pump-phase
root coverage. Separate clocks are material here: receipt can precede the source's
numerical tick. Numeric tick equality cannot materialize these roots on a recorder's
clock. The later engine-application design still needs the source-to-recorder mapping.
Ordinary exit ends the host at 889 accepted / 895 completed packets, queue/failure
zero, cuts 2/2 and four receipts received; the joiner ends at 785 / 791, queue/failure
zero, cuts 2/2 and four receipts sent. The joiner is on `ARMMAIN2.GUI` after host
leave, then exits: continued migrated play is not tested. These application counters
are not physical bandwidth measurements. Owned games/prefixes/display are removed.

The current coverage build also passes all 33 `wpn-badtgt` skirmish checks at the
standard internal speed 20 (Watch/settings starts at 10): live, puppet, restore,
fresh-process seek, inventory, address scan, effect counts and UI. Its 2,041-step
recording contains 331 owner-only named starts and refuses zero off-thread events.
The three round trips at 421/1021/1051 poison 27 tables, leave zero residue and
compare exactly before/after. Coverage reaches `ev_named` and `ev_start_site`;
this fixture does not reach the extra-weapons `tagpu_rec_start_named` helper. This
pass preserves the skirmish path. The intermittent `main+0x531` GUI-pointer
inventory failure of the earlier normal-speed control is closed by the `leave` row
`guitop`.

This carrier covers named starts and their arguments, not raw COB execution/model
heaps. Existing skirmish replay reconstructs those through engine roots under its
accepted policy. Nano sprays, replies and other owner-only events, complete Impure
state, root materialization in the engine and native multiplayer file/play/seek
remain open. No recorder exclusion, new byte patch or engine state write is added.

### Source-roster native verification

The first wire-version-2 four-actor live run produced both local sequence-1 rosters and both sequence-2 full players on each peer, but delivered no contributions: the join worker returned SDK `DPERR_TIMEOUT` (`0x887700F0`) before opening its private session. Its contribution producer stopped at tick 15. The host had opened a private session but received no bindings and exhausted its bounded 256-packet transmit queue at tick 816. This failure does not establish live source-map exchange. The adapter now names the exact failed native open stage in its error log, without adding retries, changing deadlines or imitating success. The diagnostic rerun below passes. It does not establish the timeout's cause or a by-design fix; private-session startup reliability remains open. No deadline, retry or timing mitigation was added.

MEASURED 2026-10-04 on the reference setup, a new native four-actor *Town & Country* game, both copied DLLs identical to the tested build, private display. Host human 632031350 and AI 632031347 advertise the same source roster checksum `606D73BA`; join human 632031348 and AI 632031346 advertise `3F39675A`. Each published roster equals its recipient's exact checksum at sequence 1, and all four source-tick-1 full images match raw source checksums/provenance at sequence 2. On each recipient the remote human maps source self row 0 to local row 1; the hosted AI maps 2 to 2 or 3 to 3. Host receives its remote maps at application tick 54 and join at 17: authority before arrival remains unknown. Both source seals validate. After ordinary exit the host reports 80 accepted / 86 completed, cuts 2/2 and four receipts; the join reports 54 / 60, cuts 2/2 and four receipts sent. Both end with queue/failure zero. The joiner was still on `ARMMAIN2.GUI` after host departure, but this run ends it afterward and does not verify continued host-migrated play. No game `.imprec` is created and no game playback/seek is run.

### Owner-unit native verification

MEASURED 2026-10-04 on the reference setup, another native *Town & Country* game with two humans and one hosted AI each, `each=1500`, private display. Both peers use the tested DLL. Host human 634186757 owns commander slot 3001; host AI 634186752 owns slot 1. Join human 634186759 owns slot 4501; join AI 634186753 owns slot 1501. All four first full 328-byte unit/mover images have birth 0, source tick 1 and sequence 3. Publisher and recipient raw checksums/provenance agree exactly: respectively `6A77C340`, `BE9EC6A2`, `AFC1DA8C`, `4B26CAC5`. Human owner rows map from source 0 to recipient 1, giving mapped checksums `3A79A3CE`/`FFCFBA02`; unchanged AI rows retain their raw checksums. Remote unit images arrive on the joiner at application ticks 12/13 and on the host at 59/60. Initial source rosters, full player images and seals also pass.

At tick 151 the host reports 86 sends / 70,944 application bytes, 89 receives, queue 1 and one pending completion; the joiner reports 192 / 81,584, 108 receives, queue 0 and four pending completions. These are separate observations, not a physical bandwidth measurement or an enforced budget. Ordinary exit ends the host worker with 110 accepted / 116 completed, queue 0, unread 16, failure zero, cuts 2/2 and four receipts received; the joiner ends with 225 / 231, queue 0, unread 2, failure zero, cuts 2/2 and four receipts sent. The joiner remains on `ARMMAIN2.GUI` after host departure, then exits: continued host-migrated play is not verified. Owned processes, prefixes and display are removed after the test.

This run establishes the first full live unit/mover contribution from each human/hosted AI. Synthetic controls establish delta, incarnation, death and absence semantics. Neither result proves live combat coverage, complete owned objects/Impure authority, physical bandwidth limits, native multiplayer file creation or engine playback/seek. The earlier private-session timeout remains unexplained despite the passing reruns.

### Earlier shared-session runtime evidence

These measurements precede the isolated production channel below.

MEASURED 2026-10-04 on the reference setup, retail content, native DirectPlay over loopback, two peers on *Two Continents*, private display, work branch checkpoint `d74ffebe` plus the contribution prototype. Neither peer carried `tagpu_recorder.on`. Both received the other's source-tick-1 resource sample, with energy production 25 and metal production 1; their own remote engine records still reported production zero. The two origin IDs differed, but each mapped to seat 1 on the receiving peer, while its local human was seat 0.

A real solar build on the host produced changing samples. At the tick-9151 health report the host had sent 118 packets / 11,800 application bytes with one queued, and the joiner had received 119; the reports are distinct observations, not a same-instant comparison. Both reported loss 0 and failure 0. The host's production then read 45 while the joiner's stock copy of that host still read zero. These are application payload counts for one receiver, not link bandwidth or a ten-player budget measurement. Network Pause brought both peers to tick 9193.

An armed network dump at tick 5534 contained two valid CRC-checked blocks and the local commander's one-node main list (unit slot 1501). Both remote lists (slot 1) were refused; no remote list chunk was present. The local list was preserved. This is a test of the capture boundary, not a faithful multiplayer snapshot.

The current `tools/replay/recmp_transport_test.c` exercises the actual production channel and authentication module with controlled private backend calls. Event barriers prove configuration copying, two human/AI bindings before data, packet lifetime through a held send, failure at a full 256-slot queue and refusal of another worker during teardown. Duplicate data collapses, a delayed prior-match packet cannot bind, and independent AI sequences remain separate. Its `gap`, `unauthorized` and `tail` cases reject a missing initial sequence, a wrong actual private sender and a missing last DATA before its valid final seal. `missing_end` proves an absent unsealed source cannot retire queued work; the drain deadline ends that channel with an explicit timeout failure. Start-message guard-page, span and identity controls remain. The reproducible native runner below compiles and runs all five cases; these controls do not imitate native SDK completion behavior.

A second two-peer game, using the updated adapter that copies transport outputs through aligned locals, also received both tick-1 samples and reported loss/failure 0 through tick 2551. This rerun used the same isolated prefixes after both games were stopped and relaunched with the rebuilt DLL.

### Earlier shared-session hosted-AI and order runtime evidence

MEASURED 2026-10-04 on the reference setup, retail *Town & Country*, two native-DirectPlay peers, one locally hosted AI on each, recording off. An unused lobby row becomes a host AI after `UNUSED → BLOCKED → AI:<host>`; a joiner can claim an unused row directly as its AI. The first four-seat probe had native IDs 596808690/596808688 for the humans and 596808695/596808694 for the AIs. Each human was type 1 on its own peer and type 3 on the other; each AI was type 2 on its hosting peer and type 3 on the other. The AI IDs were distinct native senders, not the human's ID reused.

The order probe exposed a duplicate physical delivery: the joiner received human source ID 598552075, slot 1501, empty main list, source tick 1, sequence 2 twice. The second receive found sequence 2 already committed. Treating physical deliveries as distinct logical transactions ended contributions incomplete. The transport now discards repeated deliveries before queue reassembly, including our own stream, and rejects source gaps/stream changes. The queue invariant was retained.

The rerun used identical copied DLLs on both peers, from checkpoint `88577130` plus the order/AI changes. Source IDs were 598995265/598995267 for the humans and 598995268/598995269 for the AIs. Both peers received source-tick-1 resources and the other's human/AI nonempty main queues at source tick 2, birth 0, sequence 4. At tick 1801 the host reported 157 sent / 25888 application bytes and 56 logical receives; the joiner reported 56 sent / 8920 bytes and 157 logical receives. Both reported loss/failure 0. These counts cover their two local contributors, not link traffic or a full-match budget.

Untimed dumps at host tick 3603 and joiner tick 3604 each had two valid CRC-checked blocks. All four commanders' main queues were present, one 88-byte node each. Each full normalized queue image was byte-identical across peers, including both hosted AIs. This is an authoritative-cache comparison; remote engine list heads were not followed.

A local ARMLAB was then created through the scenario applier and ARMPW/ARMROCK/ARMPW/ARMCK queued through its build UI. The factory's three remaining main-list nodes were present on both peers' dumps at tick 14242. The images differed only at the first node's byte `+0x0A` (162 on the owner, 161 on the recipient). This comparison is **not** equal. The cache retains source tick/sequence, while the dump stamps the recipient's current tick; file materialization must preserve source and application times, rather than claiming those two snapshots are the same state. No tolerance or exclusion was added.

Through tick 21451, with the factory producing units, both peers reported loss/failure 0 and no queue refusals. The host reported 21699 sent / 4122240 application bytes, 12790 logical receives, two queued; the joiner reported 12790 sent / 2115912 bytes, 21701 receives, one queued. Reports are separate observations. Both returned through the ordinary network Options/Exit/Main Menu path. The probe instances and private display were then removed. No network `.imprec` was produced.

`recmp_orders_test.c` passed under Wine against the actual bounded cache: complete publication after interleaved AI chunks, retention of the old queue during assembly and malformed input, missing/duplicate/stale chunks, another incarnation, capacity/slot/list bounds, a full 4096-node list, and pool exhaustion. The current transport controls exercise duplicate delivery, independent AI sequences, signed binding/source checks and a missing initial source sequence. Native transport capability and completion behavior are covered by the separate backend fixture below.

The application-tick extension passed the same cache test under Wine: source tick 9 can publish at recorder tick 8; a source-tick-10 transaction begins assembly at recorder tick 11 and publishes only with its final chunk at recorder tick 15. The old source/application ticks remain 9/8 throughout partial and rejected input, while the interleaved AI retains its own 10/11 pair. The normal parallel DLL build is clean. These controlled cache checks exercise different clocks. At that checkpoint the updated adapter had not been rerun in a native network game; the later isolated-channel game below measures its source/application ticks.

### Session lifetime audit

DISASSEMBLED 2026-10-04: `0x46BF20` tests whether `0x51E58C` is nonzero. That global is the module handle returned by loading `reporter.dll` at `0x46C007..0x46C00F`, not a match counter or a lobby identifier. `0x46BF30` resolves the module's `_RIEnable@4`, `_RIGetVersion@4`, `_RIInitializeEx@8` and other report exports. The two network cleanup callers, `0x450DD0` and `0x452370`, skip `0x4C9B70` while that handle is nonzero; `0x46C190` calls the network cleanup before freeing the loaded reporter. The earlier address range starting at `0x450D80` contained a separate function ending at `0x450DC0`; it is not the cleanup function's entry. This identifies the exception but does not establish a unique session GUID for every level. A fresh engine-start match ID supplies that identity, as described below.

### Match identity at the engine start

The host's existing start send (`0x496D25 → 0x451DF0`) now carries a packed
25-byte `0x08` message: version, byte count, host native ID and a freshly created
match GUID. Its fixed-length table initializer writes 25 at `0x512AF8` on every
initialization. This is metadata on the original start action, with no version or
capability negotiation. Every peer must run the same build, as the product contract
requires. A one-byte stock start is not the extended wire format.

The receiver observes the original `0x4553BE` case and preserves its engine action.
Before accepting a match it requires the actual delivered span, version, byte
count, nonzero GUID, actual native sender, and the bounded source player row's ID
and host bit. `tagpu_wire_received` exposes B3's existing TLS buffer identity and
delivered length; allocation capacity cannot authorize a read. Acceptance is on
the game thread in network lobby/start states 2/3, so loading/play deliveries do
not overwrite the token. The first network B2 attempt consumes it once, including
when channel setup fails. Load teardown preserves a pending token because it can
occur after the start message and before B2.

The contribution envelope's first GUID is now this **match** ID, not the native
session ID. Receive filters it before an origin can bind a stream or sequence.
This separates matches even when a lobby retains its native session. A failed
host GUID creation sends a same-size zero-ID start; the game action is retained
and contributions refuse it. At attach all five start/player-data sites are byte-matched, all four
stubs are allocated, and every page is acquired before any code is changed.
Failure leaves the stock sites together.

The transport test passes with a delayed prior-match sequence-32 packet preceding
the current match's sequence 1. The old packet is discarded before binding and the
current stream remains valid. It also checks a valid start ending exactly at a
guard page, truncated/oversize messages, wrong sender, wrong code/version/declared
length, a zero match ID and preservation of the output on refusal. A zero-ID
channel refuses setup. The held-send and source-gap tests still pass.

MEASURED 2026-10-04 on the reference setup, native DirectPlay, recording off:
two humans with one hosted AI each started *Town & Country* with match ID
`F2611633-A502-406C-ADD8-1B902E1194E4` on both peers. Human native IDs were
603832790/603832788; AI IDs were 603832787/603832786. Both received the other's
human and AI resources and nonempty commander queues. On the joiner, source-tick-2
queues published at recorder tick 1; on the host they published at tick 7. This
reruns the source/application-tick adapter in a native game. At tick 1351 the host
reported 130 sent / 21352 application bytes / 44 logical receives, the joiner
44 / 6904 / 131; both had loss/failure zero. These are distinct observations, not
a simultaneous accounting comparison or a bandwidth-budget measurement.

Both returned through the normal network menus, then the **same two processes**
started *Two Continents*. Both accepted a new match ID,
`AA5002E8-FDD9-4CDB-BF38-BDCFFAD42B07`, and restarted contributions at tick 1.
At tick 2251 both separately reported 55 sent / 8996 application bytes / 55 logical
receives, queue/loss/failure zero. This proves the fresh ID path on consecutive
starts, not native-session reuse: the native host ID changed to 604017354.
The controlled prior-match test covers an old packet ahead of the new stream.
Both copied DLLs matched the built DLL's SHA-256 throughout the runs. These games
use the default reliable engine path.

A third two-human *Two Continents* game used `-p -1` on both new launches.
Live peeks before the lobby and in play found `0x506DBC = 0` on both, confirming
the raw path. Both accepted match `5CF1B1B6-8184-428F-877E-B53166C27A10`, native
host ID 604651503, and received tick-1 resources and nonempty commander queues.
At tick 1351 the host reported 35 sent / 5636 application bytes / 34 logical
receives, the joiner 34 / 5468 / 35; queue/loss/failure zero on both. This run uses
the final source's aligned GUID output before packing the wire image. Copied DLLs
again matched the built DLL. No network recording was produced, and ordinary exit
does not prove the shared synchronous Send/Close contract.

### Send queue and shutdown constraints

The primary DirectX 7 SDK's [asynchronous messaging contract](https://library.thedatadungeon.com/directx7/devdoc/live/directx/dpover_38s9.content.htm)
says a synchronous send waits behind earlier asynchronous sends in its queue. Its
[protocol guidance](https://library.thedatadungeon.com/directx7/devdoc/live/directx/dpover_7k8p.content.htm)
also says synchronous nonguaranteed sends can block under protocol throttling.
Thus moving our own Send call off the game thread, or replacing it with an async
SendEx on TA's session, does not establish that contributions cannot delay TA's
sends. This is a gap in the checkpoint transport, not a completed fix.

The SDK's [DestroyPlayer contract](https://library.thedatadungeon.com/directx7/devdoc/live/directx/dpref_58e1.content.htm)
requires that player's pending async sends to complete or be canceled before the
player can be destroyed. Disassembly also finds two engine Close calls in
`0x4C9B70`: one for the DirectPlay2 interface and one for the original DirectPlay
interface. Deferring only the first call cannot preserve the session. Neither an
extra interface reference nor a successful ordinary exit establishes delivery at
teardown.

#### Separate-session isolation probe

MEASURED 2026-10-04 on the reference setup, two standalone 32-bit probe processes
under Wine with native DirectPlay TCP/IP over loopback. Each process had a
primary DirectPlay4A session and a second DirectPlay4A session owned entirely by
a recorder worker. The application GUIDs differed; each session allowed two
players and used `DPSESSION_DIRECTPLAYPROTOCOL | DPSESSION_KEEPALIVE`. Both
processes joined both sessions before the primary session was closed.

Each main thread called Close and released its primary interface. Only **after
that Close returned**, its worker sent a directed, guaranteed asynchronous
`AFTER_PRIMARY_CLOSE` marker on the private session. Both sends returned
`DPERR_PENDING` (`0x8000000A`); each worker observed a successful
`DPSYS_SENDCOMPLETE` and received the other worker's marker after its own primary
Close. Both processes exited successfully after an external release marker
allowed their private sessions to close. Primary Close took 2502 and 2503 ms.
Those times belong to this probe; there was no stock-only comparison and they
are not evidence that TA's traffic is unaffected.

Native `CoCreateInstance(DirectPlay4A)` returned `0x80040154` in the isolated
prefixes, including after native DllRegisterServer returned success. The successful
probe instead called the installed native module's `DllGetClassObject`, created
the requested interfaces through its class factory, and retained the module
until all interfaces and the worker were finished. It did not modify the shared
prefix registry. This is a probe creation route, not an installed recorder path.

The result establishes that a second native session can carry acknowledged
messages after the primary session closes in this configuration. It does **not**
establish TA integration, a bandwidth budget, cross-address discovery, host
migration, loss recovery, or a production drain protocol. The external release
marker held both private sessions alive until both results were observed;
elapsed time was not used to decide that delivery had completed.

Before using this direction, bind a private-session sender to its actual
primary-session human and hosted-AI identities without adding a capability/version
negotiation handshake or weakening source authority. Copy every needed identity
and address while the game owns them; the worker must retain no engine pointer
or primary-session interface. Bound pending async sends and interpret
`DPERR_PENDING` as accepted work, not a transport failure. Source-frame seals and
terminal manifests must establish completion; a timeout can mark a recording
incomplete, never prove it complete. No such adapter or marker has been installed
by this probe. Multiplayer files remain disabled.

#### Isolated native adapter and source-binding fixture

The work branch now contains `tagpu_recmp_net.c`, an engine-free native TCP/IP
adapter. It owns a separate DirectPlay4A session, player, receive event, module
reference and COM apartment on one worker. The copied configuration contains
only provider, match GUID, host role and a bounded primary-player address. No
primary-session interface or engine pointer enters this adapter. The game calls it through the copied production channel described above. Native primary interfaces are used only on the game thread to obtain the initial roster and address.

Its application GUID differs from the fixture's primary GUID. Session discovery
matches the fresh match GUID in the private session name; the session requires
the native protocol, retained message IDs and ordering. SendEx uses guaranteed
async sends, with sixteen fixed contexts. An accepted send, `DPERR_PENDING` or
`DP_OK`, transfers the copied packet to the SDK; only a validated SENDCOMPLETE
releases that context, and only success counts delivery. A seventeenth pending send and an early Close
return BUSY. An abort ends only the worker's private interface and makes no
delivery claim. The current backend supports TCP/IP only.

**The adapter's DirectPlay is always the system's**, loaded by its full path
(`GetSystemDirectoryW` + `\dplayx.dll`), whatever the game folder ships. A bare
`LoadLibraryA("dplayx.dll")` resolved to the game folder's stand-in -- TADR's
`Dplayx.dll`, the Patch Loader's `dplayx.dll` that forwards to `tplayx.dll` -- which
is not DirectPlay's COM server: every two-player network game of retail+tadr1 and
prota-4.8 crashed on both peers at EIP 0 inside the open, before `recmp: isolated
session open` (MEASURED 2026-10-09 under Wine; plain retail, with no stand-in, opened
at the same commit; which call jumped to 0 was not established). A full path names a
file: on Windows the rule that an already-loaded module of the same name wins is the
search order's, for a name without a path [INFERRED from the loader's documented rules; not
measured on Windows], and under Wine the full-path load returns
the system module beside the stand-in the exe has bound -- MEASURED 2026-10-09 on both
setups' host and joiner, `recmp: private channel on C:\windows\system32\dplayx.dll`
and then `recmp: isolated session open`, the takeover having logged the exe's
DirectPlay slots pointed at Windows' own. The adapter still checks the module it got:
one whose file is in the game's folder is refused, the open fails at `native library`
(logged), and the worker's failure refuses multiplayer recording while the game plays
on. That refusal path is the one every failed open takes; it was not provoked here.
DirectPlay loads its TCP/IP service provider itself, by the name its registry entry gives
[INFERRED: `dpwsockx.dll`, a bare name], so a game folder that shipped that file would
shadow it as the stand-ins shadowed dplayx; no setup the compatibility suite runs ships
one (checked 2026-10-09). Worker discovery retries are installed; cross-address discovery, host migration and production terminal manifests remain unverified.

The SDK's [GetPlayerAddress contract](https://library.thedatadungeon.com/directx7/devdoc/live/directx/dpref_7jqx.content.htm)
allows several local network addresses and returns both text forms. The first
adapter parser incorrectly treated another address as invalid. It now retains
up to 32 distinct ANSI addresses of at most 256 bytes each and tries that bounded
list for discovery. It also requires a bounded `DPAID_ServiceProvider` GUID matching TCP/IP, and derives the provider from that SDK address when the copied provider field is zero. It omits the primary endpoint's port when constructing the
new session's discovery address. In the successful native fixture, the host's
copied primary address was 507 bytes and the joiner's 130 bytes; both private
opens returned zero. This is local discovery evidence, not a cross-address test.

### Native player keys and signed private bindings

The primary session's native CreatePlayer initial data now preserves the
engine's 21-byte prefix and appends a 280-byte public-key record, for exactly
301 bytes. This reaches every native human and hosted-AI creation through the
same wrapper. The branch redirects `0x4CA762` to a stdcall helper and `0x454315`
to accept exactly 21 or 301 bytes. The original password/version tests and
HRESULT-to-BOOL conversion remain. Key creation failure forwards the original
21 bytes and refuses faithful contributions; it does not prevent ordinary play.
All five start/player-data redirects share one preflight and installation.

`tagpu_recmp_auth.c` creates one temporary RSA-2048 signing key per process at
native player creation, before play. Concurrent creators build independent
candidates and atomically publish one fully initialized object; losing creators
release only their own candidate. The published key and provider remain alive
for the process. Only the public key is exported; no private key is written to
a recording or disk. The CryptoAPI context uses VERIFYCONTEXT and SILENT; the
[context contract](https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-cryptacquirecontexta)
defines temporary key use without provider UI. This is network identity
metadata, with no capability/version negotiation or new primary Send in play.

The receiver obtains each public key through GetPlayerData for the **actual
primary native ID** and confirms native local ownership agrees with the engine
controller. The private worker signs the primary origin, stream GUID and its
40-byte identity (match GUID, actual private session GUID and private native ID)
with SHA-256/RSA. A receiver requires its copied primary key, the current match
and session, and the actual private SDK sender to verify that 296-byte body.
A private origin claim or a public key supplied by a private packet cannot
establish authority. The production channel freezes those verified bindings before accepting data from each remote human or hosted AI.

This replaces the proposed resource-message carrier. TA's periodic `0x28` call
at `0x4655A1` is inside the viewed-record branch, so it cannot advertise every
idle hosted AI. The other `0x4573D0` callers are state dependent. Their resource
message length and body remain unchanged.

MEASURED 2026-10-04 on the reference setup, two standalone native-DirectPlay Wine
processes using the production backend and authentication module, with one
primary human and one primary AI each. All four actual primary IDs held
301-byte initial data with the original prefix preserved. Native SetPlayerData
for a remote-owned player and native Send from a remote-owned ID both returned
ACCESSDENIED (`0x8877000A`) on both peers. Each receiver copied its two remote
public keys before both primary interfaces were closed and released.

Only after both Close/Release publications did each private worker send two
signed bindings and sixteen 80-byte data packets, eight per primary origin.
Both receivers verified the two signatures against native player keys, then
received all sixteen data packets in per-origin order. Each sender observed
18 successful completions, 2000 completed application bytes and zero remaining
contexts. Filling all sixteen contexts made send seventeen and early Close
return BUSY. Reusing the caller's packet between accepted sends preserved every
payload. The final saved-runner run measured primary Close at 2502/2710 ms;
these are standalone fixture times without a TA or stock-only comparison.

The authentication controls also pass: simultaneous key creators publish the
same immutable key; exact public and signed spans can end at a guard page;
short/overlong spans leave outputs unchanged; altered origin, stream, match,
session, private sender, signature and public key all reject. These tests run
against the actual production module, not an imitation of its parser.

The runner observes both private-ready publications before reading native
player keys, both key-ready publications before primary Close, both
Close/Release publications before private sends, and both PASS results before
private Close. These are **test barriers**, not a shipping negotiation or
terminal protocol. A deadline reports failure and never proves delivery.
Reproduce with a free discovery port:

```
python3 tools/replay/recmp_net_test.py --port 47733 --out /tmp/recmp-net-run
```

The runner compiles the backend, authentication module, channel controls and the production channel when requested, reserves
two instance directories with private registry hives and native DirectPlay,
and owns a private X display. It removes those prefixes and display after the
run and retains logs/executables in the new output directory. It never launches
TA. Earlier versions of the fixture used explicit primary `0x28` identity sends;
those established native ownership but did not establish an idle-AI carrier.
The current fixture uses native initial data instead.

MEASURED 2026-10-04 on the reference setup, a real native-DirectPlay TA game on
*Town & Country*, two humans and one hosted AI on each, recording off. Both
copied DLLs matched the tested build's SHA-256. Both peers accepted match
`5B843511-4C85-43A3-84B2-D24AA124FCE7`. Each copied all four 301-byte native
player keys and confirmed native ownership against its controller: human IDs
612385515/612385513 and AI IDs 612385518/612385519. Both reached play and
received the other human/AI resource samples and nonempty commander queues.
The source-tick-2 queues published at recipient ticks 2 and 6. At tick 151 the
host reported 21 sent / 3040 bytes / 19 logical receives, the joiner
19 / 2704 / 21; both reported queue/loss/failure zero. These are separate
observations, not a bandwidth measurement.

That game verifies the bootstrap patches and native human/AI key route. Its
contributions still used the old shared-session channel; it does **not** verify
the isolated adapter inside TA. No multiplayer file, record/play/seek fixture
or landing review has passed. The test instances and private display were removed.

### Isolated production channel verification

MEASURED 2026-10-04 on the reference setup, two native-DirectPlay Wine processes, the production `tagpu_recmp.c`, backend and authentication module. Run with `--channel` to create and close both primary-session stand-ins **before the private channel is started**, with all primary keys and addresses already copied. The caller zeroes its configuration and payload after enqueue, proving their retained copies. Both peers receive sixteen 80-byte DATA packets, eight per actual remote primary human/AI origin, in source order. The host completes 22 sends / 3440 application bytes and the joiner 18 / 2000; the additional sends are signed identity reannouncements. Both finish with pending, queued, unread, lost and failure zero. The primary Close measurements, 2508/2516 ms, are standalone fixture values without a stock comparison.

```
python3 tools/replay/recmp_net_test.py --channel --port 47737 --out /tmp/recmp-channel-run
```

Both PASS observations precede a test release barrier that permits private Close. Those test barriers are not a production terminal protocol. The fixture proves the actual channel can initialize and deliver without a live primary interface; it does not run TA or create a recording.

MEASURED 2026-10-04 on the reference setup, actual TA on *Town & Country*, two native-DirectPlay humans and one AI hosted by each, private display, recording off. Both copied DLLs match the tested build's SHA-256. Match `1BD93A08-04F8-4AC4-B605-DDB0B99E84C0` has primary human IDs 614038678/614038676 and AI IDs 614038675/614038674. The recorder's private IDs are 614058660/614058662. Both peers validate all four primary keys/ownership and bind both remote origins to the other actual private SDK ID. Both receive authoritative human/AI resources and nonempty commander queues; source-tick-2 queues publish at recipient ticks 11 and 54.

At tick 151, host statistics show 23 accepted DATA / 3376 bytes / 17 logical receives and 29 native completions / 5536 bytes. The joiner shows 18 / 2536 / 30 and 20 completions / 3256 bytes. Both report bound 2/2, pending, queued, lost and failure zero. These are separate observations, not link bandwidth or a traffic-delay comparison. This verifies the isolated channel inside TA. It does not prove ordinary-exit terminal completeness, host migration, a file or playback. Owned instances, prefixes and the private display were removed.

### Channel failure handling

MEASURED 2026-10-06 on the reference setup under Wine, five Impure peers each seating one AI on *Core Prime Industrial Area*, only the host recording; two-peer games always completed. The faults below ended five-peer channels; each paragraph gives the rule the code now states for it.

**Opening: the listener-port race.** Microsoft's TCP/IP provider (`dpwsockx`) picks its stream listener's port by walking up from 2300. Several processes starting it at the same moment can pick the same port; the loser's session enumeration fails with `80004005`, at once or after about 1.5 s of the provider's own retries (Wine socket traces). Five peers lost 19 of 70 opens, in 12 of 14 rounds; no peer lost twice, and every loser opened on its next attempt. Two peers lost none. `tagpu_recmp_net_open` reports a `DPERR_GENERIC` from enumeration or Open, the two calls that start the listener and both before any session is open, as `RECMP_NET_PORT_RACE` (`HRESULT_FROM_WIN32(WSAEADDRINUSE)`, `80072740`); that a host's Open starts the same listener is [INFERRED]. The worker opens again at most ten times. This is a **bounded retry to get the session open, not a safety argument**, and the owner accepted it on 2026-10-06 as a documented timing residual. A failed open leaves nothing behind, so a retry admits nothing a first open would not. The bound rests on each lost race meaning that another peer now holds that port [INFERRED from the traces], with at most nine other peers. **The residual hole:** a peer that loses ten races fails its channel, and every other peer then fails as unbound (below).

**Binding.** Data waits in the 256-slot transmit queue until every remote actor the roster names has bound. The count of bound actors never falls and no data is sent before it is complete, so a queue that fills while an actor is unbound has never been sent from: the channel fails as `RECMP_UNBOUND` (`DPERR_NOCONNECTION`, `887700AA`) and logs the primary IDs that never bound. A game that stops before binding completes fails the same way. A full queue with every actor bound is still `E_OUTOFMEMORY`: the private session is not accepting sends. In the five-peer runs a lost open left its peer's two actors unbound, and every other peer's queue filled and refused with `8007000E` under the old name *queue full*.

**Sends after a departure.** At the end of a five-peer game the host broadcast into a session whose remote players had all left, while its worker still counted three peers, and SendEx answered `DP_OK` instead of `DPERR_PENDING`; the backend mapped that to `8000FFFF` and failed the channel. `DP_OK` is now an accepted send like `DPERR_PENDING`: its context stays held until its SENDCOMPLETE. **The invariant:** a context is never reused while DirectPlay may still complete it. MEASURED: more than 600 `DP_OK` sends in the port-race probes and 80 after clean leaves, each with exactly one matching completion. `recmp_net_test.c`'s backend host now broadcasts after the joiner has closed and before receiving its departure notice; on the old backend that send fails `E_UNEXPECTED`, on the new one it completes once and the session closes cleanly. A completion that never arrived would hold its context and refuse Close, which fails the drain incomplete; it can never free a context early.

**Consumption ends; the channel does not.** At that game's end each joiner's engine cleared every other remote player's row (active 0, ID `0xFFFFFFFF`, controller 0; [the engine map](../exe-reverse-engineering.md) has the removal `0x452CC0`) while those players were still sending. On every joiner the next player image from another joiner then failed the seat test at GameTime 666..669, and the refusal aborted the whole channel (`80004004`). The joiners' private players left before sealing what the host had consumed, so the host held seals for only five of its eight sources and its recording could not complete. A peer that cannot apply what it receives now keeps contributing:

- The game thread's refusal of a remote packet calls `tagpu_recmp_relinquish` (`tagpu_recorder.c:mp_relinquish`) instead of aborting the channel. It takes nothing more and ends its own recording (`tagpu_recmp_file_end` with `channel_ended` 0). Its sends, seals, final seals and receipts continue, so the other recorders' completeness does not depend on it. A failure of this peer's own production still aborts the channel, since a source that cannot produce must not seal successfully.
- Consumption is a one-way latch on the channel with three reasons: the consumer relinquished it, the worker found the 256-slot receive queue full, or the game ended with no recording attached (a failed journal detaches itself and counts as none). A full receive queue ends consumption, never the channel. At stop with no recording attached, nothing would read the queue again, and the packets still-playing peers send used to fill it in about 24 ticks and fail the channel.
- **The ordering** that keeps what was consumed a contiguous prefix of every source: the worker is the receive queue's only producer, tests the latch before every publication, and the latch never clears. Once one packet goes unpublished, no later packet is published. The worker still validates every packet against its source's ledger, and still counts seals and sends receipts: those are this peer's contributions to the other recorders.
- `tagpu_recmp_take` answers `-2` once consumption has ended. `tagpu_recmp_cut` and `tagpu_recmp_file_complete` refuse, and the end-of-game check stops requiring cuts, since no recording takes this channel's cut any more. Seals and receipts still gate this peer's finish.
- Every refusal names its reason in the log: the worker's refused packet (malformed, unauthorized, out of sequence, a receipt for no final seal), the end of consumption with its reason, the recorder's refused remote packet (`mp_refuse`: the packet's origin, kind, sequence and what refused it), a refused local production at a capture point or at the level's end, the unbound primary IDs, and each lost port race.

**Owner ruling, 2026-10-06: not a loosening.** No completeness check accepts anything it refused before. A recording whose consumption ended is refused by the cut, by file completion and by the writer, where the failed channel used to refuse it; the only change is that the other recorders no longer lose this peer's contributions with it.

`recmp_cut_test.c` covers this at the production channel with a controlled backend. In `relinquish` the consumer stops after two packets and the remote source that is still active never seals. In `rx_full` 300 packets arrive before the game takes any. In `no_consumer` the journal detaches before stop and 300 packets follow. `journal_full`'s failed journal now behaves as `no_consumer`. In each, the channel finishes without failure: both local final seals go out, all four receipts return, nothing is published after consumption ends, and the cut is refused. The five-peer game under the new rules has not been run yet.

### Gaps before the first code landing

- Verify cross-address discovery, player leave and host migration. The separate application/session removes the shared receive-signature and shared send-queue obligations by construction, but does not prove physical network bandwidth cannot delay game traffic. Measure and enforce that budget.
- Integrate the frozen consumption cut, validated final receipts and retained consumer into the file writer. Preserve semantic application history and partial-transaction boundaries through teardown and seek. Packet-prefix/transport closure is not durable-file completion.
- Carry the remaining player authority and materialize contributed resources/orders; retain source tick, sequence and recorder application order in the file and its seek snapshots.
- Decide unit corrections from a bandwidth measurement, as the product plan requires; no measured budget is inferred from the two-commander run.
- Run the network recorder/playback fixture and the skirmish replay gate on the finished accumulated diff. The transport test and network dump do not replace those gates.
- **Stage 6 residuals of the channel's failure handling.** *The pre-binding queue window:* data waits in the 256-slot transmit queue until every remote actor binds, so a peer that never binds (no Impure, an open that failed, a departure before binding) fails every other peer's channel as unbound once its queue fills; admission control, which would decide who is expected, is stage 6's. *The post-departure send stall:* after a peer departs, sends stalled for 11–15 s before completing (MEASURED 2026-10-06, five-peer runs), holding their contexts meanwhile; how a recorder treats leavers is stage 6's.

## Work order

1. **Network capture boundary.** Prove the lifetime and bounds of objects read from type-3 unit owners, especially the air order proxy whose owner can delete it independently (`tagpu_recorder.c`, *What it reads*; [order research](demo-recorder-s2-orders.md#2-orders-on-a-type-3-puppet)). Map the capture tick against the network pump and owner sends. A probe must distinguish a remote copy from an authoritative local record. The header currently writes `RT_MODE` as `MODE_SKIRMISH` (`tagpu_recorder.c:header_build`), so a network file also needs a deliberate format rule. A file with plausible but wrong remote state is not a completed landing.
2. **Contribution envelope and authority.** Give each contribution an explicit version, bounded length, origin transport ID, source tick and sequence, and recorder application order. Map source ticks to the recording's ticks; the paused peers above differed by one. Capture owner-authored resources, orders and other missing state, and verify delivery with two peers before relying on any field. Continuous presentation samples can be superseded; discrete orders, selections and chat require ordered delivery or repair ([protocol constraints](demo-recorder-exploration.md#perspective-data-and-network-budget)).
3. **Perspectives and communication.** Record explored-map history, camera, cursor, clicks, selections, public/team/private chat and pings with their audiences. Sight is recomputed from recorded units, as the product contract specifies; it is not contributed. S5's UI work consumes some of these streams; the recording format has one owner at a time, coordinated before `tagpu_recfmt.h` changes.
4. **Playback and takeover.** Host up to ten recorded seats without assuming a spare viewer seat ([puppet-host limit](demo-recorder-s2-host.md#13-the-limit-a-ten-seat-recording-new)), restore each seat's controller and authoritative state, then take over one player with the others as AI. Run record, puppet, seek and takeover checks on a real two-peer game and extend them to the ten-seat case. Preserve the existing replay round-trip gate for any simulation change.

Step 1 is the research gate. The work branch enables observed native capture with the separate authority ledger from step 2. No multiplayer landing is complete: it needs the native fixtures and a checked account of every remote field it carries and applies. Presentation and playback remain required; a partial file must not be presented as a faithful multiplayer replay.


### Ordered owner root contributions

Production wire kind 10 uses one 4,096-slot copied game-thread queue for six
existing roots: `RK_EV_CREATE`, `RK_EV_DEATH`, `RK_EV_DEATH_END`,
`RK_EV_START_NAMED`, `RK_EV_REPLY`, `RK_EV_SPRAY`. It preserves
mixed call order, including A/B/A actor changes, and packetizes only contiguous
same-origin, same-source-tick entries. A u32 count precedes packed twelve-byte
prefixes: u16 kind, u16 payload span, boolean pre-B2 phase, three zero reserved
bytes and the exact incarnation token. Each existing root payload follows
unchanged. Up to 167 minimum twelve-byte reply payloads fit a 4,032-byte body.
Four-byte death returns permit at most 251 entries; each kind checks its own
payload length before reading any field.
Script names/arguments retain the legacy bounds; replies have kind <24, boolean
root/text flags, zero reserved bytes and at most 128 non-NUL text bytes; sprays
are exactly 48 bytes with a boolean reverse flag and signed low layer word 0..9.
The high layer word is retained unchanged, as in skirmish replay.

Successful owner creates use the existing 28-byte payload: shared unit ID,
nonzero u16 raw type, source owner row, layer 0..3, all 32 finished bits,
three position dwords, water 0..4 and three zero reserved bytes. The source
owner row must map to this origin's frozen destination seat; copied bytes are
retained in source coordinates. The later engine consumer must also bound the
type against its definition count. The success-exit observer records the return
even with recording off, after any constructor replies, with B4's exact birth
token and source capture phase. A distinct contribution-only root `0x100`
records the same arguments at `0x486075`, after reservation/stamping but before
initialization and child roots. Its fixed owner pointer is assigned when the
unit array is allocated, not by the constructor's overwritten owner argument.
The whole nine-byte observer span contains no relative instruction. One queue
retains begin, child calls and successful return in source order; playback need
not look ahead to a later return to discover that incarnation. Initial units
remain in the initial full images. These kind-10 subtypes leave wire version 2
and file header formats 5/6 unchanged; older root decoders refuse unknown
subtypes. An earlier recording without begin roots still reads as captured
data. This supplies the owner-lifetime boundary, not engine playback.

Owner deaths carry the existing 340-byte payload: unit ID, killer unit ID,
source killer-player row (ten for none), raw severity/kind bytes, carried flag,
and the complete normalized 328-byte victim/mover image at destructor entry.
The image's owner ID and owner row must match the source roster and origin's
frozen block; player indices, killer ID and live bit validate before publication.
The four-byte return carries the same unit ID and exact birth in its prefix.
Both recording-on and recording-off owners use the existing `0x4866D0`
entry/return observer. Entry saves the bounded source identity before the
destructor clears its slot; return publishes that saved identity without reading
the freed mover or cleared birth stamp. The entry image is protected by the
same game-thread destructor lifetime, not a pointer probe. Frame exhaustion or
queue failure makes capture incomplete. These roots do not yet carry the
owner-only feature/effect work inside a death or enable native engine playback.

The entire copied batch validates source roster/stream, frozen origin block,
span, kind, birth/phase and provenance before publication. There is no partial
cache update. Existing unit/player array bounds and the same game-thread level
lifetime protect capture; replica controllers are filtered before reading caller
text or spray arguments. Nano credit reuses the query/handler/point invariant.
An uncredited multiplayer spray refuses contributions rather than claiming an
unknown owner. Hooks copy without a worker call or lock. Prior-tick calls drain
before sealing, current pump calls after advancing, and final calls before final
seals. Overflow, backwards source time and failed emission latch incomplete.
The extra queue replaces the former named-only queue; there are no independent
per-kind drains that could regroup calls. The constructor-begin observer adds
one byte patch and no engine field write.

MEASURED 2026-10-05 on the reference setup: `recmp_roots_test.py` passes 83 atomic
refusals, all 1,300 guard-page truncations of three/four/six-kind mixed batches,
485 guarded per-kind short spans, maximum 167 replies and 251 death returns,
exact future incarnation tokens, constructor/finished/high layer bits, source
owner-row authorization and unchanged unknown outputs. Queue controls verify
8,392 copied mixed calls over phase/tick changes,
actor switches, packet boundaries and ring wrap; overflow/backwards clock/failed
emission permanently refuse. `recmp_file_test.py` writes a 3,964-byte
synthetic world/container with 27 application records and seven late proof
records. C and independent Python preserve three actors' exact ordered
create/death/return/script/reply/spray batches through reading and seeking, including phases, future
birth tokens and high layer bits. Late source proof changes reply text from OK to
NO while leaving application authority unchanged; proof-only roots establish no
applied authority. All five lifecycle controls, 23 header refusals and 109 body
refusals with valid raw seals/cuts pass. `recr_mp_roots` returns the last applied
copied batch separately from `RecWorld`; the ledger keeps every batch.

These controls establish bounded capture representation and reader history.
They do not materialize engine roots or map independent clocks. Actual native
file capture is verified separately below; engine multiplayer playback/seek
remains refused. Remaining owner-only roots and declared Impure authority,
all POV/communications, budget enforcement,
ten seats and continued migration remain required.


MEASURED 2026-10-05 on the reference setup: native *Two Continents*, two humans
and one hosted AI each, 1,500 slots per actor, private display and recording off.
Both peer DLL copies match the tested build at launch. The registry requests 20,
but live heartbeats report speed 10; this is normal-speed evidence. Sixty combat
units exercise script starts, while commanders/solars placed near each local
starting base exercise reclaim replies and sprays. All four frozen source
rosters, full player images and 74 observed full unit-image contributions match
the recipient's source bytes/provenance. All twelve first root-type observations
(four actors times three kinds) match source packet count/span/CRC, event index,
unit ID, incarnation and phase:

| Origin / owner | Root | Source tick / sequence | Calls / bytes / CRC | Event / unit / birth | Recipient tick |
|---|---|---|---|---|---|
| 643537554, host human | reply | 74 / 29 | 2 / 52 / `C00B0F5F` | 0 / 1523 / 74 | 34 |
| 643537554, host human | script | 135 / 282 | 3 / 121 / `F17B7956` | 1 / 1518 / 74 | 95 |
| 643537554, host human | spray | 161 / 366 | 6 / 265 / `2CFB5415` | 1 / 1523 / 74 | 120 |
| 643537559, host AI | reply | 74 / 31 | 2 / 52 / `E93418D6` | 0 / 4513 / 74 | 34 |
| 643537559, host AI | script | 121 / 194 | 1 / 53 / `2403BA92` | 0 / 4509 / 74 | 81 |
| 643537559, host AI | spray | 436 / 1325 | 3 / 157 / `9B4AB2B8` | 0 / 4502 / 0 | 396 |
| 643537552, join human | reply | 48 / 26 | 2 / 52 / `B934165A` | 0 / 23 / 48 | 92 |
| 643537552, join human | script | 98 / 252 | 3 / 121 / `0ED95145` | 1 / 21 / 48 | 141 |
| 643537552, join human | spray | 127 / 331 | 6 / 265 / `82A2D4AA` | 1 / 23 / 48 | 170 |
| 643537558, join AI | reply | 48 / 28 | 2 / 52 / `2AF7ABD1` | 0 / 3013 / 48 | 92 |
| 643537558, join AI | script | 94 / 190 | 1 / 53 / `E4DA3F24` | 0 / 3010 / 48 | 137 |
| 643537558, join AI | spray | 170 / 391 | 1 / 64 / `B8894F4D` | 0 / 3013 / 48 | 213 |

The six-call human spray packets include mixed kinds, with the first observed
spray at entry one. All these first observations have post-B2 phase zero;
guarded codec/queue/file fixtures cover both phases, but this native sample does
not establish pre-B2 root coverage. Source/recipient clocks differ by up to 44
here; numerical tick equality is not an application mapping. Ordinary surrender
closes the host before the joiner exits: 2,862 accepted / 2,868 completed,
queued/failure zero, cuts 2/2 and four receipts received, remote final sources
0/2 and 36 unread proof-tail packets. The joiner remains on `ARMMAIN2.GUI`, then
exits with 2,875 / 2,881, queued/failure zero, cuts 2/2, four receipts sent and four
unread packets. No file is created, so no durable tail or native replay is claimed.
These counters are not physical bandwidth. Both owned games/prefixes and display
are removed. The first fixture attempt put reclaimers near combat: most were
killed before producing sprays, while contributions remained healthy. The revised
fixture observes all three root types without changing capture semantics.


The current coverage build passes all 67 `battle` skirmish checks at internal
speed 20, including live recording, puppet playback, in-place restore, fresh
seek, AI takeover, timeline forks/export/deletion and live/paused rewind.
The live recording ends complete after 4,080 iterations, with zero off-thread
events and zero uncredited sprays. Its 4,079 checked steps contain 23,051
owner-only script starts, 17,106 replies and 3,302 sprays; every recorded change
is explained. Round trips at 721/2101/2641 poison 26 tables, leave zero residue
and compare exactly before/after. Coverage reaches all six shared capture
functions (`ev_named`, `ev_start_site`, `ev_reply`, `ev_nano_query`,
`ev_nano_handler`, `ev_spray`) and the inactive multiplayer guard/push/end
functions. This preserves the skirmish path; it is not native multiplayer
engine application coverage and does not resolve the intermittent GUI-pointer
inventory failure seen in a previous normal-speed control. No exclusion or
tolerance changes. The 39 harness logic tests pass, the driving skill validates,
and the wiki builds 98 pages plus its index with the new sections inspected.

### Ordered application reader

`recr_mp_apply` reads every produced/consumed envelope after an acknowledged
ordinal through the current `RecWorld` tick. It preserves recorded global order,
source clock/sequence/stream, application clock/phase and the exact batch bytes.
It visits complete semantically validated batches, including unit/player/queue
updates and source seals, so the eventual engine consumer can retain their causal
order. A latest-owner cache cannot supply this sequence: the raw ledger is its
source. Proof records never visit. The callback borrows bounded copied file data
until it returns and cannot re-enter the reader.

Inspection queries and application delivery have separate bounded histories and
block cursors. Neither can consume the other's data; both restore the world's
next file offset after reading. They rebuild after an index/segment change or a
backward world tick. The consumer supplies its acknowledged ordinal, setting zero
for a new reconstruction; a resumed ordinal reconstructs validation from the
start while skipping already delivered envelopes. Repeating a delivered prefix
visits nothing. An ordinal beyond the current world's available prefix refuses
before any callback. Each returned callback advances the acknowledgement before
a later read can fail. This is engine-free reader state, with no format change,
new byte patch or engine field write.

MEASURED 2026-10-05 on the reference setup: the actual writer's synthetic fixture
is 3,512 bytes with 27 application envelopes, ten remote consumptions and seven
late proof envelopes. Six mixed root batches include three updates at tick 101
in A/B/A owner order; each has script/reply/spray calls with distinct exact payloads.
The C cursor's trace matches independently decoded Python envelopes, including
both clocks, ordinal, phase, origin, sequence, kind, span and whole-packet CRC.
Byte-exact C copies compare complete sequences after repeated polls, an ordinal-10
resume, interleaved latest-state queries, replay from zero and backward/forward
world seek. An out-of-range acknowledgement visits nothing. Five file lifecycles,
23 malformed-header and 74 raw-seal-valid semantic-body refusals pass. The reader
also opens/inspects a real skirmish `battle` recording and returns no multiplayer
applications at its first world tick. No game launches in this fixture.

### Native observed-world file capture

`mp_record_begin` attaches `RecMpFile` before the first publication/consumption
at B2, after the native pump. It transfers ownership to `RecRing` before starting
the writer and records mode 3, copied transport metadata and the frozen seat/block
mapping. The primary stream retains this recorder's observed unit/mover/player
records, tables, globals, projectile pool and explored map. Only local controller
1/2 queues enter that stream: the owner pointer is first bounded to the ten player
records; controller 3/4 heads are never followed by capture or rotating verification.
The separate contribution ledger retains every owner's queues and exact source
incarnation. It does not attach them to an unknown/lower-bound replica incarnation.

Owner-only script starts, replies and sprays have one representation: contributed
ordered batches, including local publications. Their primary-stream event copies
are suppressed in mode 3. `s_rec_fired` stays false for native files, preserving
native fire sends/filters; it is true for skirmish capture. Live rewind entry,
flush and suspension remain skirmish-only. No new engine patch, field write,
simulation declaration or recorder exclusion is added.

The game producer borrows the file context only until `mp_record_end`. It clears
that borrow before detaching the game-owned journal and publishing context end;
the writer cannot free the context before acquiring that publication. Local
save failure publishes local-stop, leaving mandatory contributions active and
preventing the writer from taking the channel consumer. Ordinary level teardown
ends the channel before publishing context end, enabling sole-consumer transfer
and proof-tail draining behind the final application cut. Capture prerequisites
or allocation failure leave contributions running without a local file.

MEASURED 2026-10-05 on the reference setup, two native Wine humans with one hosted
AI each, *Two Continents*, 1,500 units per actor, normal DLL: host recording on,
joiner recording off, with ARMFLASH/CORAK combat groups and ARMCOM/ARMSOLAR
reclaim probes. All four source rosters, full player images, first unit
images and twelve first script/reply/spray observations match recipient bytes
and source provenance. The host produces a 2,524,535-byte mode-3 file covering
GameTimes 1–536, with 6,697 ordered applications (3,314 remote consumptions).
C and independent Python readers validate the complete footer/source prefixes,
all four player histories and owner unit-block bounds. Sequential delivery and
world seek retain exactly 1,673,880 application bytes, 1,073 root batches and
trace CRC `4D695660`. The observed stream has no duplicate copies of those three
owner-root kinds. The writer reports 536 iterations and zero corrections.

The departing host closes while the joiner remains on `ARMMAIN2.GUI`: 3,383
accepted / 3,389 completed, queued/failure zero, cuts 2/2 and four receipts,
remote final sources 0/2. The joiner exits later with 3,369 / 3,375,
queued/failure zero and cuts 2/2. Only the enabled host saves a recording.
Registry speed 20 was requested, but native heartbeats report 10; this is a
normal-speed measurement. The reader command is
`tools/replay/recmp_file_test.py --native-file FILE --out DIR`.

A coverage-build local-save failure attempt reaches the writer's explicit failure
at tick 1, but its joiner's private SDK discovery fails at session enumeration
with `80004005`. The host never binds, fills the bounded 256-packet transmit queue
and refuses with `8007000E`. This is an unsuccessful contribution run and adds a
startup reliability sample; it proves no failure-isolation pass. The later
normal-build run validates all four actors' source/recipient script/reply/spray
bytes after the local writer has already failed, then ordinary exit cuts 2/2
and four receipts. A passing rerun does not explain either this enumeration
failure or the earlier private-session join timeout. Neither transport behavior
nor queue size is changed to hide the failure.

The corrected normal-build failure/transition run deliberately places a regular
fixture file at `replays`, so the local writer cannot open its path. It ends at
tick 1 with explicit `REC_END_WRITE`; all four actors continue contributing exact
roster/player/unit/root bytes. Host ordinary exit finishes with 1,590 accepted /
1,596 completed, queued/failure zero, cuts 2/2 and four receipts while the joiner
stays in play. The joiner later finishes 1,672 / 1,678, queued/failure zero,
cuts 2/2. After removing only that owned fixture file, the same host process
starts a skirmish with recording still enabled. The complete mode-2 file ends
at GameTime 1,293 with zero corrections, 908 shots and 1,985 primary script roots,
no contribution blocks. Heartbeats confirm internal speed 20 and about 60 ticks/s.
This verifies local file failure isolation, native-to-skirmish recorder reset and
restored skirmish fire capture. The first normal transition driver's multiplayer
exit helper expected `TABMENU.GUI` on the skirmish's `ARMOPT.GUI`; correcting the
fixture to the documented skirmish menu path produces the completed recording.

Owner snapshots retain source clocks and source-local linked state. Replacing
observed images with an unchecked union of owner images would not establish a
coherent world. The field-authority/clock materialization invariant, remaining
owner roots/effects/Impure authority, native engine playback/seek/takeover,
POV/communication, measured traffic budget and ten-seat/migration gates remain
open. The accepted skirmish policy reconstructs COB roots and fresh AI; it does
not preserve raw COB/AI execution heaps. File completeness and reader seek do
not establish faithful engine playback.


The current coverage-build `secondary-orders` skirmish regression passes all
23 checks at internal speed 20: complete recording to GameTime 1,140, every
one of 1,139 changed steps explained, live/puppet/world/radar comparisons,
root/sink counts, images inventory, address scans and no COB engine writes on
puppets. Round trips at 421/451/541 poison 26 tables, leave zero residue and
compare before/after exactly. Fresh-process seek at 391 also matches. This
checks the shared capture/header/start/stop path; it is not native engine
playback coverage or an explanation of the previous GUI-pointer inventory
failure. The actual writer's five synthetic lifecycles, all header/body refusal
controls and the complete same-process skirmish reader check also pass.

Coverage reaches `header_build`, `rec_start`, `rec_begin`, `rec_end`,
`capture_lists`, `cap_orders`, `cap_verify`, `ev_put`, `tagpu_rec_capture`,
`tagpu_rec_level_end` and `mp_record_end` in that skirmish regression. It does
not reach `mp_record_begin`; successful native coverage and engine application
fixtures remain required for the completed multiplayer landing. Normal-DLL
native file/failure/transition runs above verify those behaviors directly.

### Replica creation roots and incarnation joins

The native format carries `RK_EV_NET_CREATE` (kind 51), which renumbers no existing
record and does not change the contribution wire. B4's existing `hit_created` callout
at `CreateFromNetwork`'s success exit `0x48634F` calls the recorder after storing
the observed incarnation token. It adds no competing byte patch. The recorder
copies only on its native recording game thread, after bounding the unit pointer
to its level's fixed array. It copies the constructor's type and position from
record offsets `+1` and `+5`, its owner from the bounded unit/player records,
water state from unit `+0x10A`, and B4's exact/lower-bound/unknown token unchanged.
It also copies post-B4 unfinished fraction `+0x104` and HP `+0x108`:
`kill_apply_state` has already applied valid carried values at this point.
The 32-byte event contains u32 unit ID, u16 type, u8 owner/water, u32 birth,
three i32 position arguments, float unfinished fraction, u16 HP and zero u16
reserved. It retains primary-world creation order; it does
not manufacture an owner creation at a guessed recorder tick.

C and Python readers require the native format, unit-def metadata, an allocated
nonzero slot in the frozen owner's block, type in `1..ndefs-1`, owner below ten
and water in `0..4`, a finite unfinished fraction in `0..1` and zero reserved
bytes before exposing the event. File reads and seeks retain its
arguments and token; no engine memory is written. Native engine playback remains refused, so the new
record is not sent through the skirmish event dispatcher.

MEASURED 2026-10-05 on the reference setup: the actual writer's 3,554-byte
synthetic fixture retains the network creation arguments, lower-bound birth
token, fraction and all HP bits exactly. C and independent Python refuse fifteen
container-valid malformed length/slot/owner-block/type/water/fraction/reserved/
format cases before publishing the event. A
format-5 copy without the new record remains readable; the existing complete
format-5 skirmish recording passes reader/summary/full-header inspection. All
five file lifecycles and the existing contribution controls pass.

MEASURED on the same date, normal DLL, native *Two Continents*, two humans with
one hosted AI each and 1,500 slots per actor: the host records while the joiner
opts out. The complete 1,807,645-byte format-6 file covers ticks 1–354 with zero
writer corrections, 4,086 applications/1,970 remote consumptions and 710 owner
root batches. C/Python application delivery and world seek retain 1,080,706 bytes
with CRC `54E2916B`. The primary stream contains 35 replica creation roots
(23 remote-human, 12 remote-AI); both readers retain their ticks and complete
arguments through seek, root trace CRC `FF466A0F`. Every root's type, owner,
incarnation, unfinished fraction and HP matches the next B2 unit image and birth
table in this sample. All 35 fractions are zero, confirming these are carried
finished creates rather than the stock constructor's unfinished default.
The existing image/event checker explains all 353 transitions, including the
35 local and 35 replica creates. This does not check application of the separate
owner ledger into the engine. Actual native heartbeats report speed 10.

Host exit completes 2,116 accepted / 2,122 completed, queued/failure zero,
cuts 2/2 and four receipts while the joiner stays on `ARMMAIN2.GUI`; the joiner
later completes 2,078 / 2,084, queued/failure zero and cuts 2/2. The first coverage-build
attempt failed private session creation with `80004005`, ending the local
recording explicitly incomplete after two iterations; that attempt supplies no
successful native coverage. The normal run does not explain that startup failure or
the earlier enumeration/join failures. All owned prefixes/displays are removed.

The current format-6 coverage build passes all 23 `secondary-orders` skirmish
checks at internal speed 20: complete 1,140-iteration recording, every one of
1,139 transitions explained, live/puppet/world/radar comparisons, root/sink
counts, images inventory, address scans and no puppet COB engine writes. Fresh
seek and round trips at 451/481/541 pass; each round trip poisons 26 tables,
leaves zero residue and compares exactly. The 39 harness logic checks pass.
This verifies the changed shared file path. The native materializer remains
unverified. No exclusion, tolerance or expected-absent rule
is changed.

An audit of the earlier native format-5 recording finds 3,878 individual owner
roots: 2,231 local and 1,637 remote roots have an exact live unit incarnation at
their recorded application image. Four local and six remote roots find an absent
unit; no live lower-bound/other-birth mismatch occurs in this sample. The four
local replies are kind 3, null-text root 0, and matching primary local creates
occur in the same step. Of the six remote replies, four are kind 3/null-text;
two are kind 5 with text `Reclaiming`. Their matching replica images first appear
three to seven recorder ticks later. The file's 35 primary create events all
belong to the recorder's local human/AI (22/13); it contains no replica create
event. These are B2-image measurements, not proof that all roots are safe when
the step's events run.

The replica root closes that capture gap. It does not settle the earlier owner
reclaim replies' materialization clock or authorize dropping/retiming them.
The playback consumer still needs causal ordering and an exact incarnation
identity across creation, recycling and seek, plus the previously named field
authority, owner effects, perspectives, traffic and ten-seat/migration work.

### Native startup and capture verification (format 6)

The default isolated-adapter fixture now supplies the logging stub used by
`tagpu_recmp_net_open`'s failure diagnostics. Previously that stub existed only
under `RECMP_CHANNEL_TEST`; the default build failed to link with an undefined
`tagpu_logf`. Its common stub now logs in both modes, while the worker-finished
event remains channel-only. The actual default native fixture passes its bounds,
source/cut controls, four human/AI public keys and signed bindings, sixteen data
deliveries after primary Close and eighteen successful native completions per
peer. A separate simultaneous-start probe holds both primary sessions live and
releases both private workers through an observed file barrier; it also passes.
These successes do not explain the earlier intermittent creation, enumeration
or join failures. No retry or timing mitigation is added to the adapter.

MEASURED 2026-10-05 on the reference setup: the current coverage DLL, SHA-256
checked in both owned prefixes and retained with its matching linker map, runs
the four-actor *Two Continents* combat/reclaim fixture. Only the host records;
the joiner contributes with recording off. The complete native file is
2,338,188 bytes, GameTimes 1..446, with zero writer corrections. All four source
rosters, player images, unit images and script/reply/spray first batches match
publisher/recipient bytes and provenance. Host exit completes 2,780 accepted /
2,786 completed, queued/failure zero, cuts 2/2 and four receipts while the joiner
stays on `ARMMAIN2.GUI`. The joiner later completes 2,939 / 2,945,
queued/failure zero and cuts 2/2. This verifies the host's frozen recording cut;
continued recording after host migration is still required.

C and Python agree through sequential reading and fresh world seek on all
5,620 application records, 1,422,940 application bytes, 1,030 root batches and
CRC `A9913E6F`. All 36 primary replica roots retain their tick/body bytes, CRC
`1DDD61AA`; each type (`unit+0xA6`), owner, birth, unfinished fraction and HP
matches the next B2 image. Owners 1/3 have 23/13 roots. Thirty-five are finished
creates; one is unfinished with fraction 1 and HP 0. The world checker explains
all 445 transitions: 35 local creates, 36 replica creates, 20 deaths and 552
shots, among the other recorded roots.

The matching coverage map identifies 674/540 reached functions in host/joiner.
Both reach `hit_created`, `tagpu_rec_network_created` and `mp_capture`, verifying
the active replica capture path. The adapter is outside the instrumented map;
its successful native Open is runtime-log evidence, not a coverage claim.
Native heartbeats still report speed 10. The owned games, prefixes and display
are removed. Engine playback remains refused pending the materialization,
incarnation and authority work above; this capture/reader verification does not
meet the complete multiplayer landing gate.

### Owner constructor dependency verification

MEASURED 2026-10-05 on the reference setup, the current coverage DLL runs a
four-actor *Two Continents* combat/reclaim game with host recording and joiner
recording off. All sixteen first create/script/reply/spray observations match
source and recipient batch bytes, checksum, source sequence/tick, call index,
birth and phase. The host writes a complete 1,401,962-byte file at GameTimes
1..266 with zero corrections. C and Python retain all 3,052 ordered applications,
806,806 application bytes and 508 root batches through sequential reading and
fresh world seek, CRC `AEDF9750`. The 36 primary replica creation roots retain
their exact ticks/body bytes, CRC `D68B60BD`. The observed-world checker explains
all 265 transitions, including 35 local and 36 replica creates.

The mandatory owner ledger carries 70 successful constructor calls: 22/13
for each human/hosted-AI pair, including the non-recording peer. Sixty-eight
have finished=1 and two finished=0; all are post-B2 in this sample. Every call's
exact origin/slot/birth joins a carried live source image with the same raw type
and source owner. All 35 local observed-world constructor payloads equal their
mandatory contribution payloads byte for byte. Eight other owner roots precede
their constructor's success exit; the carrier preserves that order. This proves
why a creation dependency needs incarnation indexing, and does not justify
dispatching roots solely by recorded arrival or numerical tick equality.

The retained matching coverage map shows 662/529 reached functions on host/joiner;
both reach `ev_create`, `mp_roots_push`, `mp_roots_log` and `mp_capture`. The host
ends before the joiner with 1,509 accepted / 1,515 completed, cuts 2/2 and four
receipts; the still-playing joiner later ends at 1,677 / 1,683 and cuts 2/2.
Both queues and failure codes are zero. Owned games, prefixes and display are
removed. Native speed is still 10. Continued migration, independent-clock/root
materialization and faithful engine playback remain open (mode-3 playback is
now the [materializer's](#owner-authority-materializer)). No exclusion, tolerance, timing mitigation, byte patch or engine
field write is introduced.

The same current coverage build passes all 23 `secondary-orders` skirmish
checks at internal speed 20: live/puppet/restore/fresh seek, event/image
comparisons, inventory, address scans and puppet write checks. Round trips at
331/391/511 poison 26 tables, leave zero residue and compare exactly before/
after. This verifies the shared constructor observer with the native channel
absent; it does not replace the accumulated landing replay gate or native
engine playback tests.

### Constructor and replica arrival causality

MEASURED 2026-10-05 on the reference setup, an independent audit of the
1,401,962-byte constructor-contribution recording joins each owner root by
origin/global slot/exact birth to the recorder's B2 world image at its application
tick. Of 2,135 calls, 2,073 already have the same live incarnation; 62 target an
absent slot and none targets a different live birth. Fifty-two absent-slot calls
are constructors themselves. Eight are kind-3 replies without text preceding
the corresponding constructor success exit in the source's retained call order.
Two are later, nonempty kind-5 replies:

| Origin / slot / birth | Owner constructor source / application tick | Reply source / application tick / text | Primary replica-create tick |
|---|---|---|---|
| Join human `657375758 / 4522 / 31` | 31 / 75 | 32 / 76 / Reclaiming | 78 |
| Join AI `657375752 / 12 / 31` | 31 / 76 | 32 / 76 / Reclaiming | 80 |

These two calls are after their owner's successful constructor, in different
source batches. They cannot be treated as replies produced inside that
constructor. At their recorded application tick the recorder's replica has not
been constructed. Across all 70 owner constructors, primary creation minus
application tick ranges from -1 to 8. For remote births the primary creation
minus source tick ranges from 43 to 53; this includes differing replica-arrival
delays and establishes no fixed source-clock offset. The one primary create
without a streamed owner constructor is the initial join commander, slot 4501,
birth zero, supplied by its initial full source image.

This is product decision 7: owner-controlled unit lifetimes permit these private
roots to retain their recorded arrival order; retaining the recording peer's
delayed replica lifetimes requires an explicitly chosen event-delay policy.
The owner chose owner-controlled unit timing on 2026-10-05: creation and death
follow the unit owner's contributions; private roots retain their recorded
application order. The materializer must enforce incarnation dependencies
without dropping or retiming roots or guessing source-clock offsets. That
engine application remains unimplemented and native playback remains refused.
Death capture below supplies the required lifetime observations.

The constructor-begin contribution now provides the ordering missing from an
exit-only observation: reservation and exact B4 birth precede initialization,
private roots and successful return. Both C and Python use the same constructor
payload bounds and frozen source owner-row authorization for this new subtype.
The existing primary/skirmish creation event remains at the successful exit.
No source clock offset, replay exclusion, application delay or engine field
write is introduced. The engine consumer must still construct/apply the owner
world and enforce these incarnation dependencies before mode-3 playback opens.

#### Constructor-begin verification

MEASURED 2026-10-05 on the reference setup: the native coverage fixture
`tools/replay/recmp_create_begin_test.py` runs two humans and one hosted AI per
human on *Two Continents*, with only the host saving. All 28 first observations
(four actors times seven root kinds) agree between source and recipient on
whole-batch bytes/CRC, source clock/sequence, event index, slot, birth and phase;
each also matches its persisted application envelope. The complete
2,845,687-byte recording spans ticks 1..558 with zero writer corrections.
Seventy-one constructor begins precede matching successful returns with exact
28-byte payloads, source tick and capture phase. Eight reply roots occur between
their own begin and success; the queue preserves those calls without moving
them to another clock or looking ahead to the success record. All four actors,
including both owners on the peer whose saving is off, have matched pairs.
Coverage reaches `mp_create_begin`, `create_image`, `ev_create`,
`mp_roots_prepare` and `mp_roots_publish` on both peers in the retained build map.

The independent C/Python reader run retains all 8,146 application envelopes,
2,061,238 exact bytes and CRC `8EA54D8E` through fresh reader world seek.
Twelve per-origin explored-map queries and forty local-origin comparisons
against the captured primary map also pass. This does not rerun or close the
previously stopped primary-receive exploration driver gate, or prove remote
engine map application. The native fixture requests internal speed 20, but
heartbeats show 10 at about 30 ticks/s; this is normal-speed evidence. The
initial native fixture attempt was a driver failure: its generated nickname
exceeded the native ten-character field. Shorter fresh names fix that driver;
both attempts clean up their own instances/display.

The copied-root controls pass 83 atomic refusals, 1,300 guarded batch truncations,
485 per-kind guarded short spans and 8,392 ordered copied calls including begin.
The actual writer's synthetic fixture carries the seventh subtype through both
readers, all five file lifecycles and source/application/proof histories. Fifteen
additional raw-seal-valid begin corruptions bring semantic body refusals to 135;
wrong owner rows, IDs, type, phase, birth, span, layer/water/reserved bytes and an
unknown adjacent kind refuse in C and Python. The first expanded codec run
correctly refused a reused sequence in its positive test; fixing that test to
advance the sequence passes without weakening validation.

The shared constructor payload helper also passes all 23 `secondary-orders`
skirmish replay checks at internal speed 20: complete 1,110-iteration recording,
1,109 explained steps, puppet playback, restored/fresh seek, inventory and COB
write checks. Three round trips at 391/421/511 poison 26 tables, leave zero
residue and compare exactly before/after. This verifies the shared skirmish
path; it does not establish native engine play/seek/takeover. Owner-world
materialization, complete state/effect authority and the rest of the multiplayer
landing remain open. No exclusion, tolerance or simulation classification is
loosened.

### Ordered owner lifetime consumer

`tagpu_recmp_lifecycle.c` supplies the copied lifetime actions for the engine
adapter. It consumes the semantically validated envelopes from `recr_mp_apply`
starting at ordinal one, with its own seat/unit/root baselines and source ledger.
It does not read engine memory, merge an observed replica into owner authority,
or enable mode-3 playback. Other feature bodies retain their own consumers.

Each owned slot is unknown, absent, creating, live or dying. A first applied full
unit image bootstraps an initial live unit or establishes absence. Later images
must match the completed owner lifetime's exact birth and type. A constructor
begin reserves a fresh lifetime before its private roots; success checks the
same source clock, capture phase and first 24 argument bytes. The water byte is
copied unit state, not a constructor argument: both observations survive, and the
success action supplies its own bounded value. A death entry retains its copied
normalized victim image and keeps the target available to private children;
the return action supplies that image for one destruction after those children.
The engine adapter applies this recipe under the rule in *Exactly-once private
children* below. This consumer performs no construction or destruction.

Constructor owner rows, death killer rows and the death image's player IDs are
mapped through the frozen source roster. Every image/constructor type is bounded
by the recording's definition count. The whole copied batch, including every
incarnation dependency, validates before any action callback. A later bad entry
emits none of the batch's actions. A callback failure permanently poisons the
consumer; already applied effects cannot be retried. A fresh seek needs a fresh
consumer and an application cursor reset to zero. Proof tails validate copied
source bodies but emit no actions and establish no lifetime. Completion requires
an applied baseline for every owned slot and no open construction/death bracket;
a file's transport footer alone does not prove those requirements.

MEASURED 2026-10-05 on the reference setup:
`tools/replay/recmp_lifecycle_test.py --native-file <recording> --out <directory>`
passes against the complete constructor-boundary recording. The C consumer and
an independent Python file audit agree on 10,372,503 action bytes, CRC
`8CF737F2`: four initial live units, 71 begins, 71 successes, 24 death entries,
24 returns, 20,878 live image actions, 6,020 absence actions and 4,129 private
roots. Eight replies run inside their construction bracket and five
`TargetCleared` starts inside their death bracket. All 6,000 owned slots have an
applied baseline. Five proof records emit nothing. A fresh consumer rebuilt from
the middle world seek and advanced to the end produces identical actions,
provenance, final lifetime phases and bytes. This is copied dispatch evidence,
not native engine playback or engine Seek.

Controlled cases exercise mapped cross-owner fields, different water values at
constructor begin/success, valid nested children, missing/mismatched brackets,
wrong birth/phase/arguments/type/owner, unannounced image lifetimes, a bad final
entry after valid earlier entries, unchanged output after refusal, no retry after
callback failure and proof-only initial/future images. The first compile failed
on the nested fixture's main-name macro; making the outer entry name explicit
fixes that fixture without changing production validation. Normal and coverage
DLL builds pass. The engine materializer, complete authority and remaining
multiplayer gates are still open.

### Exactly-once private children

On a puppet level the recording is the **sole producer** of an owner's private
children: named starts, replies and sprays. The adapter constructs a contributed
unit at `CREATE_BEGIN` through the constructor with the COB-write and reply gates
**shut**, dispatches each bracketed `ROOT` through the replay's own calls
(`ev_start_named`, `ev_reply`, `ev_spray`), and runs the native destructor at
`DEATH_END` with that action's saved image. Skirmish `make` opens the gates
instead because its exit-only create has no live target for the children
recorded before it; multiplayer's begin bracket gives those children a target,
so opening the gates as well would produce each one twice.

Nothing native can produce a second copy:

- **Replies, SET, ATTACH, DROP.** Every gate predicate refuses a call on a
  puppet seat's unit unless `s_open` is set, and only the replay's own reply call
  sets it (`tagpu_puppet.c`, *The gates*). With the gates shut the Create script's
  first slice still runs; its engine writes are refused, so the state they would
  write must come from the owner's images.
- **Owner-only starts.** Direct-call reachability from the constructor
  `0x485F50` and the destructor `0x4866D0` over the retail executable reaches
  three of the thirteen sites (engine map, *Lifecycle children and the engine
  consumer*). The constructor's `0x489800` calls cannot start a script: each
  follows `0x48A160`, which resets that slot's target words just before.
  The destructor's cargo hit reaches `0x489CE0`, which the hit gate refuses for
  every puppet victim whatever `s_open` holds. Its order purge reaches `0x489800`
  only for a node without `+0x42` bit `0x10000`, which every puppet node holds
  set. The order handlers and AutoAim never run for a type-3 owner.
  The enumeration does not follow the 118 and 131 indirect calls reachable from the
  two functions. The counts close that gap: `tarec compare-counts` and
  `tareplay`'s R/Q check now fail on any count at one of the thirteen sites
  in a puppet process.

A skirmish death is recorded at its entry, so the children it nests are replayed
after the destructor and find no unit: they occur zero times on the puppet. In
multiplayer they are dispatched once, before the native destruction. The
destructor deletes the COB at `0x486D8A` within that same call.

MEASURED 2026-10-05 on the reference setup, from the retained `wpn-badtgt`
coverage run: the live process counts 331 native owner-only starts by
GameTime 1,051 (163 HitByWeapon, 163 TakeDamage, two TargetCleared in `0x4898B0`,
three in `0x48A1E0`). The puppet's dump at 1,051, the round trips' dumps and the
fresh seek's count none. Feeding the live dumps in as the puppet's makes
`compare-counts` report all four sites. This verifies the existing skirmish
invariant only; the multiplayer adapter that relies on it is unimplemented.

### Owner-authority materializer

`tagpu_replay.c` plays a mode-3 recording through a materializer over the
skirmish replay: *THE MULTIPLAYER MATERIALIZER*, *THE CLOCK* and *THE DEATHS ON
A MULTIPLAYER LEVEL* in the source. Every actor is a puppet seat beside V, and
each namespace has exactly one source:

| Namespace | Source on the puppet host |
|---|---|
| Unit existence and lifetime, every frozen block | the lifecycle consumer's actions (`tagpu_recmp_lifecycle.h`); primary create and death events are not dispatched |
| Unit record and mover | the latest applied owner image of that exact incarnation, written by the skirmish image recipe |
| A unit's sight stamp and water state | the same composed image (`books_put` reads it) |
| A unit's state byte `+0x10E` | the same composed image, written by `books_put` through the setter `0x48B090` at every capture point (R8b's `book_state`, which a seek runs too); the primary's state events are not dispatched, since the recorder's copy takes each change at its own receipt time, and the puppet's state gate refuses every other change of a puppet's byte on the segment -- the create's activation `0x486148`, a script's SET -- so each change starts `Activate`/`Deactivate` or `StartBuilding`/`StopBuilding` once |
| A unit's build finish | the same composed image: the fraction `+0x104`, `+0x110` bits 13 and 14 and `+0xF5`; the unit's composite frame (its model's `Object3do+0x10`, which no image can carry) cleared by `unit_put` when the image changes whether the build is finished; the primary's build done (`RK_EV_BUILT`) is not dispatched |
| Owner private roots (named starts, replies, sprays) | `ROOT` actions at their ledger position, the recorder's own publications included; primary copies are not dispatched |
| An actor's player record | its owner's player images, mapped to recorder rows; `+0xF0`, the players phase's deadline, stays the host's own |
| A player record no actor holds | the primary stream (the destructor's rank update writes every record's `+0x148`, `0x486BE7..0x486C13`) |
| Order lists | owner order caches whose birth matches the unit's; a create and a death empty the slot's lists |
| Feature records, cells' defs and heights, the wreck pool, every corpse, projectiles, globals, declared tables | the primary stream, as the recorder observed them; corpses as `FR_CORPSE` roots |
| Cells' unit slots and yard bit | the host's engine (below); reproduction's source test takes the recorder's slot (`FS_SOURCE`) |
| Explored map per player | per-origin contributions |

**The state byte.** Played from the primary's state events, a unit's `+0x10E`
followed the recorder's clock while the comparison and a seek took the owner's
image: MEASURED 2026-10-06 on the four-actor recording, an armmex finished by
its owner (slot 4503) held the byte 1 in its owner's image from 1199, while the
primary's state event came in step 1202 and its build finish in 1203, so the
forward dumps at 1199 and 1202 differed from the expected world by exactly that
byte and a fresh seek to 1200 did not. With the composed image as the byte's
one source, the same recording played forward compares EQUAL at 300 and at
every tick from 1198 to 1203, a playing in player 1's perspective at 1200 and
fresh seeks to 1200 and 1202 EQUAL too. The forward playing, stopped after its dump at 1203, wrote 15 state
bytes from the images and the gate refused 13 changes: the creates' activations
`0x486148` of the loader's three commanders at GameTime 1 and of nine finished
units the owners made at 55, 61 and 76, and one build finish `0x41B9BF` whose
primary event came before its owner's image (slot 3006, 615). A seek's rebuild
has its creates refused in the same way (16 in a fresh seek to 1200, 13 of them
the rebuild's) and R8b writes the composed byte after them. The primary's build
done is not played on such a segment (*The build finish*, below), and a puppet
reaches `0x41B8D0` by no other path, so the gate's refusals there are the
creates' alone.

**The build finish.** The finish `0x41B8D0` writes, on the built unit, the
fraction `+0x104` = 0, `+0x110` bits 13 and 14, `+0xF5` and (through the
setter) the state byte -- every one a record field the owner's image carries on
the owner's clock -- and clears the unit's composite frame `[+0x9E]+0x10`, the
host's cached 1x bake ([engine map](../exe-reverse-engineering.html), *The
composite and the build fraction*). The nanoframe look itself, the scaffold and
the wire in both renderers, follows `+0x104` alone. The composite is what no
image can carry: a host heap pointer in the host's own model instance. Its bytes
depend on the fraction only through whether it is 0.0 (a nanoframe's bake has a
depth plane and every visible piece, a finished unit's only the cached pieces),
and the draw never rebakes a finished structure that still has one. So the
primary's build done, the recorder's copy finishing at its receipt of the
engine's `0x12` on its own clock, is not played (`mp_owned_event`), and
`unit_put` clears the composite where the image's write changes whether the
fraction is 0.0, as the engine's own round robin pairs the two for a remote
copy (`0x48B4A6`). On such a segment that write is the fraction's one writer
after a unit's create (a puppet runs no build step, `0x48AEB5..0x48AECD`), so
every bake follows the last class the images wrote, and a seek -- whose creates
start every unit with no composite -- holds the same at every draw. When the
image finishes a unit the viewer has selected, `unit_put` sets the GUI word's
refresh bit as the finish does. The playing's `owners:` line counts the clears
("N builds finished and M unfinished by an image"), and the events line reads
`built 0/0`.

MEASURED 2026-10-06 (a native two-peer recording read with `tarec` and
`recmp_file`) under the build that still played the primary's build done:
three remote finishes (slots 1514, 1502, 1503, type 78) reach their owner's
finished image at application ticks 405, 929 and 1583, and the primary's build
done came in steps 409, 933 and 1588 -- 5, 5 and 6 frames in which a finished
unit kept its nanoframe bake. The recorder's own three finishes (slots 3, 2, 4)
were in the same step. The owner image's `+0x104` reaches 0.0 together with
`+0x10E` = 1 at those ticks. A build done that comes two or more steps before
its owner's finished image would have left the nanoframe bake for the rest of
the level: the image writes the fraction back, the next draw bakes a nanoframe
again, and the finish image leaves that bake in place.

MEASURED 2026-10-06 with the clear in place (`c8d3cafb`), on a fresh two-peer
recording: the probe that reads a recording's finishes found 5 -- 3 remote ones
with a window between the owner's finished image and the primary's build done
(the widest, slot 2 type 78, image at 1018 and build done at 1025) and 2 in the
same step, none stuck. The forward playing logged "5 builds finished and 0
unfinished by an image" against the probe's 5, `built 0/0`, and played to its
end; its dumps at 1017, 1018, 1025 and 1026 were EQUAL, as were a fresh seek to
1018 and a round trip at 1025. The five-peer ten-seat recording logged 5 builds
finished and 0 unfinished. No tareplay fixture is a multiplayer segment, so
these playings are the only ones that reach the new lines.

The composed world is a materializer-owned copy of `RecWorld`'s unit, live,
player and list arrays; the reader's world stays the delta baseline. Every ID
an owner image carries is bounded as skirmish bounds it, and an image applies
only to a slot whose lifecycle phase is live with the same birth. A unit absent
from the recorder's start world is made from its owner's first image
(`BOOTSTRAP`). In the first sample one remote actor's commander (slot 4501) is
absent, arriving later as a network replica; the other remote actor's (slot
1501) is in the start world. The recorder holds every remote player, human or
AI, as controller 3, and the materializer admits no other remote type: the
explosion's owner gate tests 3 alone (`0x49A043`).

**The clock.** At the recorder's B2 for tick T, `mp_capture` (1) flushes the
post-capture roots of T-1, still labelled T-1, (2) advances, (3) flushes the
pre-capture roots of T, (4) publishes the local players' images of T, (5) takes
the remote packets, consumed at T, and (6) publishes the local units', order
lists' and explored map's images of T, which equal primary image T.
So the replay applies, before image T, every envelope labelled below T and
those labelled T up to the first produced root batch whose first entry is
post-capture. It applies the rest of T after the primary events that follow
image T. Applying all of T after the events instead left every local unit one
tick behind its image (footprint `+0x78`, sight stamp `+0xF8`).

**Cells' unit slots and the yard bit.** A cell's slots name the unit standing
on it, and the yard bit is a structure's. The recorder's grid names its replicas
at its own times, while the host's units follow their owners. No recorded slot
is the truth on such a level, so the footprint writers' writes stand
(`tagpu_puppet_begin`'s `engine_cells`), and `cell_put`/`slots_put` write
neither. Every slot the host's grid names is a live host unit: the engine writes
it, and its destructor takes the unit off the grid (`0x48682D`) before it clears
the alive bit (`0x486DCE`). `recmp_compare.py` checks that instead of comparing
the slots, and checks that every set yard bit lies inside a live structure's
footprint (`+0x76`/`+0x78` by `+0x7E`/`+0x80`, `+0x110` bit 29) instead of
comparing the bit; the owner approved that check on 2026-10-06. The feature
code reads one slot, reproduction's source test (`0x424189`); a multiplayer
recording records the value the recorder's tick read there (`FS_SOURCE`, an
`RK_EV_DRAW` inside the tick), and the host's stub at `0x424185` answers from it
(`tagpu_replay_source`).

**The deaths.** A unit's end is its owner's: `DEATH_END` runs the destructor
through `ev_death` at the owner's time, with the feature gates shut and the
record's corpse level (`+0xA`'s low nibble) cleared, so it changes neither the
grid nor the pool. The destructor reads that nibble only to call the corpse
helper `0x486360` (`0x486D55..0x486D6F`, its only caller). A death's feature
effects are the recorder's. The recorder records its corpse call as a root,
`FR_CORPSE`: the unit's type, the level, the smoke flag, `+0xFF`, the cell
`+0x76`/`+0x78`, the position and the turn — everything `0x486360` reads. The
host makes it through `0x486360` on a stack record holding exactly those
fields, at the root's recorded phase and place, like the explosion's feature
roots. On such a level the grid's and the pool's structure therefore change only
through the recorder's roots, in recorded order: a spawn takes the free list's
head (`0x423DA8`) and the list mover pushes at it (`0x4232F0`, LIFO), so every
spawn meets the free list the recorder's met and takes the record, the cell and
the list place the recorder's took. The pump `0x4954C8` runs before the capture
`0x4954ED`, so a remote unit's death is recorded LATE, after its iteration's
tick root, and its corpse goes in there.

**Named residuals.**
- *The split.* A remote unit's explosion plays at its owner's time and
  position; its corpse, flare and smoke appear when the recorder saw the death,
  in the recorder's replica's cell — 2 to 8 ticks later in the samples. In the
  other order the unit stands over its corpse until its owner's death. The owner
  accepted this on 2026-10-06: deaths keep the owner's clock and features the
  recorder's.
- *Orders naming a unit its owner destroyed.* Another owner's order list can
  still name a unit for those same ticks, until that owner's next image drops
  it. The host's destructor has already cleared every reference to it, and
  `ord_target` links no unit the host does not hold, so the order names no
  target here. On a puppet the order is inert. `recmp_compare.py` expects no
  target for such an order (the owner approved this on 2026-10-06). Those
  refusals are part of the playing's "order nodes … refused" count.
- *A player who leaves.* Its owner sends no `DEATH_END`, so its units stay alive
  on the host from their last images while the recorder's kill-all makes their
  corpses. A rule that ends a departed owner's units is a lifecycle landing of
  its own (the owner's decision, 2026-10-06).
- *A death's other events* stay unplayed, as their owner contributes them; a
  draw a destructor made outside any root is counted. Projectiles come from the
  images.
- *Shots.* They come from the recorder's observation, while their units'
  positions come from owner images, so a remote unit can fire from where the
  recorder's replica stood.
- *The first owner image.* Until a remote unit's first owner image applies
  (ticks 54–57 in the first sample), it holds its start image, and the host's
  engine derives another water state, which is written back from the image each
  step (the units' bookkeeping counts in the playing's log).
- *Passability.* The pathing contexts on the host follow the host's slots
  (`0x440A40` → `0x47DFC0`), a cache nothing on a puppet host simulates with
  [INFERRED].
- *The builder's half of a finish.* The finish's GUI effects for the builder --
  `0x4199B0`, which relabels the top GUI window's unit buttons with a tracked
  builder's queued counts, and the GUI word's refresh bit for a selected builder
  -- are not made: the image write that clears the composite names only the
  built unit. [INFERRED invisible: the viewer, record 10, owns no unit and has
  no build menu up.] An owner root for the build done would make them on the
  owner's clock; it still needs the clear at the image write, since a root can
  apply one application tick before its finished image.
- *No tareplay fixture reaches the clear.* `unit_put`'s new lines run only on a
  mode-3 segment, which no `tareplay.py` fixture is; `recmp_play_test.py`'s
  forward playing exercises them, as it does the materializer's other
  mode-3-only code.
- *The host leaving first.* MEASURED 2026-10-07 in three-peer games whose
  host quit before its joiners: once the host's own recording closed normally
  (942 iterations), and once, on the previous build with the same channel
  code, the host stopped consuming when its receive queue filled during the
  quit and its writer abandoned the file at its last complete block. Ending a
  recording when its own player leaves is the leavers' lifecycle stage.
- *Native DirectPlay's `DP_OK` from an asynchronous send.* The channel keeps
  the send's context until its completion arrives, which Wine's DirectPlay
  posts for every such send (MEASURED: more than 600, each with one
  completion). Native Windows is unmeasured. Were it to post none, each such
  send would hold one of the 16 contexts until the channel refused further
  sends and the recording ended incomplete at its drain deadline: no corruption,
  a lost recording.
- *One channel worker per process.* A match that starts while the previous
  match's worker is still draining (at most its 15 s deadline after that level's
  end) starts no channel on that peer, which logs "the previous match's worker
  has not ended"; every other peer's channel then fails as unbound, naming it.
  Letting two workers coexist belongs with the traffic stage.
- *The browser validates a multiplayer file whole.* The Replays browser's
  summary of a multiplayer file scans every contributions block, so that no span
  is shown for a file whose completeness is not proven; it runs on the game
  thread each time the list is built or the selection changes, and its cost
  grows with the file. Reading the proof from the end block alone is the
  remedy.
- *A count comparison for mode 3.* If one is ever added, `0x41B8D0` runs 0 times
  on the host by design, like the state events; an expected-absent row would be
  a loosening, the owner's decision at that time.

MEASURED 2026-10-06 on the reference setup, internal speed 20, a native
four-actor game recorded with the corpse root (`recmp_create_begin_test.py`: two
humans and one hosted AI each; four 1,500-unit blocks; GameTimes 1..520) and
played by `tools/replay/recmp_play_test.py`:

- The recording holds 25 deaths, 23 with a corpse level, and 23 `FR_CORPSE`
  roots, each the last root of its death, each at its death image's cell. The
  recorder's own corpses are at phase 0 and the remote ones at LATE; 14 remote
  deaths applied by their owners 3 to 8 ticks before the recorder saw them, and
  11 in the same step. The attach log reads "recorder: 53 event sites in".
- The level plays to its end with no stop: 72 units made, 25 destroyed, 18,988
  images, 3,552 private roots, all played; 23 corpses made from the stream. The
  features line reads 0 out of bounds, 0 returned otherwise, 0 draws not taken
  as recorded, 0 cells and 0 wreck records left for their structure, the heads
  differed 0 times. COB engine writes and native owner-only starts on a puppet:
  0.
- `tools/replay/recmp_compare.py`, with no exemption, against dumps at
  GameTimes 120, 216, 259, 291, 302, 351, 411, 423, 424 and 519 — inside
  owner-first windows (259, 291, 411), at the recorder's own deaths with
  explosion roots (302, 351, 423, two in one step) and after the rank update
  (216): EQUAL at every one, units, players, order lists, features, the pool,
  the cells and the yard bits included.
- **Reproduction.** A second native game was recorded with
  `tools/reproduce_fixture.py`'s archive on both peers (`--reproduce --until
  2000`; GameTimes 1..2091). It holds 18 `FS_SOURCE` draws, all inside the
  tick's root and all reading slot 0, at the 18 trees at rest that the cursor
  passes (ticks 387 and 1577..1957). Every one spawned its child in rows
  668..670, which is the row bug. The recording also holds 39 deaths, 37 corpse
  roots and 0 corpses outside a death. Played with the same archive
  (`--content`), it reached its end: 0 draws not taken as recorded, 0 cells or
  wreck records left for their structure, the heads differed 0 times, and 37
  corpses were made. Dumps at 388 (after the first spawn), 1578, 1722, 1744,
  1958 and 2090 are EQUAL. 388 had one difference before the comparer expected
  no target for an order naming a destroyed unit (above): unit 10's order named
  slot 1505, which its owner destroyed at 388 and the recorder saw at 392. The
  positive list's change for `guitop` refuses these recordings (a recording
  names its list); a third, recorded on the current list (GameTimes 1..2071,
  42 deaths, 40 corpses, 18 source tests), is EQUAL at 193, 205, 246 and 338
  (inside owner-first windows), 388, 1578, 1958 and 2070, and its log names
  each anomaly kind: three lines per call site, the rest counted. No
  native sample reads a nonzero source slot, so the branch where the
  recorder's slot differs from the host's runs only through the same stub.

### The seek of a multiplayer recording

`seek_run` seeks a mode-3 recording within a playing (the `seek` trigger, the
replay HUD, a round trip) and from a fresh process (`tacli replay seek`). A live
multiplayer game has no rewind, so the rewind's return to now refuses one. *THE SEEK OF A MULTIPLAYER RECORDING* in
`tagpu_replay.c` holds the argument:

- **The composed world of iteration T is rebuilt, not carried.** After
  `recr_seek` stands the reader at T, `mp_rebuild` restores the level-start
  composed world (copied when the materializer starts), starts a fresh
  lifecycle consumer, reads every envelope from the first (`recr_mp_apply`
  from ordinal 0) and applies them with `mp_apply_held(T - 1, 1)` and
  `mp_authority`, the calls `mp_step_image` makes when the world holds
  iteration T. So the composed world, the consumer and the held queue are the
  forward playing's at that point by construction: the same reads, the same
  order, the same split at T's first post-capture root batch. Only the actions
  differ. They compose data and touch no engine state (`mp_compose`), and a
  private root is not played, as a skirmish seek plays no event before T.
- **The seek makes the composed world.** R7 makes every unit it describes
  (`level_units` on `mp_world`), and a unit its owner created but has not yet
  described from its create's own arguments and return (`mp_make_held`). R8–R14
  write the composed units, lists and actor players, and the primary stream's
  features, globals and tables. Two exceptions keep the host's own state. Each
  seat's players-phase deadline comes from the recorder's image, because it is
  the host's clock and an owner's image carries its own machine's. The cells'
  unit slots and yard bit stay the host's (`sk_features` under
  `s_engine_cells`): R3's kills took every unit off the grid, and R7's creates
  and R8's moves put the composed world's units on it.
- **A refused seek** reads the stream back to where it was (`sk_back`), and the
  materializer is rebuilt there by the same rule. A composition that fails
  stops the playing (`mp_fail`), so nothing composed by half is applied.
- **The reader.** A snapshot of a mode-3 file is followed by its contributions
  block before the stream goes on, so `recr_seek` steps over contributions
  entries when it checks that the stream follows the snapshot. `tick_read`
  already steps over every block that is not the stream's.

`recmp_compare.py` checks the slots more strictly since this seek: a slot must
lie inside the footprint of the live unit it names, and every cell of a live
mobile unit's footprint must name it. A structure's yardmap leaves some of its
cells open, such as the four corners of slot 4502's 6×6 footprint at 2070.
Without that check, a seek that wrote the recorder's slots compared EQUAL:
unit 3006 stood in the recorder's cells, one row off its owner's, and its old
row named it after its death.

MEASURED 2026-10-06 on the reference setup, internal speed 20, the reproduction
recording on the current list (GameTimes 1..2071):

- **In-process seeks:** backward from 900 to 246 (inside an owner-first window)
  and forward from 1000 to 1838. Dumps are EQUAL at 246, at 366 (120 ticks on),
  at 1838 and at 1958, and the playing goes on to its end after both seeks.
- **Round trips** at 193 and 338 (inside owner-first windows): 26 tables
  poisoned, residue 0, A against B EQUAL.
- **Fresh-process seeks** to 74, 75, 246, 338, 1578 and 1958, the last two
  across the snapshots at 901 and 1801: each dump EQUAL. The fresh
  seek to 1958 took 407 ms, 402 of them reading the stream and composing the
  contributions.
- The skirmish `secondary-orders --cover` regression passes all 25 checks on
  the same build.
- **Not run:** a seek between a unit's create and its first image (0 such
  units at every seek point). In these recordings, a create is applied after
  its capture, or with its image in the same one.

### The takeover of a multiplayer recording

`tacli replay takeover` and the `takeover` trigger take a mode-3 recording over
as they take a skirmish's ([S4b](demo-recorder-s4-takeover.md)): at GameTime T
player P is human, every other recorded seat a fresh AI, the viewer's seat
dropped, and the game after T is recorded as a skirmish timeline of the same
file (owner decision, 2026-10-06). The takeover is the seek in its TAKEOVER
mode, so it runs on the composed world of the seek above: `tk_bound` bounds
P's live count and every seat byte in the composed player images, and
`tk_orphans` tears down the lists of host units the composed world does not
describe. Nothing else in the takeover's steps is changed: TK1's seat flip
comes after R3's kills and before R7's creates, so every unit is made a local
mover whichever machine owned it in the recording.

**The materializer follows the segment** (`mp_follow`, after every reader
seek). The takeover's timeline was recorded by a live skirmish, every unit its
own seat's, so a playing of that timeline plays it as a skirmish's: the
recorded world whole, the cells' slots and yard bits the recording's
(`tagpu_puppet_engine_cells(0)`). In the file's own segment the materializer
composes T's world and the cells are the engine's. The boundary's seek to
`fork_at` crosses into the timeline, and a seek back before it composes
again. `recr_segment_mp` names the segment the world was read from.

**R3b, the cells after the kills** (`sk_cells_clear`). Where the cells are
the engine's, the seek has relied on R3's kills leaving no unit on the grid,
since a death clears the footprint the unit stands in. That holds only while
the engine has owned the cells all along. After a playing of the takeover's
timeline, the cells were the recording's, and a death left the recorded cells
that named the unit somewhere else. MEASURED 2026-10-06: a seek from 1166 in
the timeline back to 900 left slots 3005 and 4502 in four cells outside their
footprints. So with every unit dead, a slot naming no live unit is dropped and
no yard bit stands. R7 and R8 then stamp the composed world on an empty grid.

**What the comparison expects.** `recmp_compare.py --fork N` compares the
takeover's first iteration with the composed parent at `fork_at`, less D2's
list (`tarec.fork_view`). The grid's slots and yard bits are checked on the
fork's iteration, as on a dump. The contact bits `+0x110` 8–10 and 12 are the
viewed seat's scan (R14b), and an owner's image carries its own machine's: a
player's own units are in contact on its own machine. So when both worlds view
the same player, a composed unit takes the recorder's bits from the primary
stream. A unit that stream does not hold yet is compared without them and
counted. Both measured forks counted 0. A seek in that view, and a change of
view to it, take bits 8–10 from the primary stream itself (R14b,
`sight_change`), on every slot live in the primary's image whose host slot holds
a live unit of its type: that is all `rw_contacts` checks, not the unit's
identity. The host's unit comes from the composed world and the bits from the
primary, so where the primary's image of a slot still describes an earlier unit
of the same type than the one the composed world put there, the new unit shows
that unit's contacts until the viewed record's next radar pass — display state
on a puppet host. A dump comparison takes the same rule
whenever the dump views the seat the recorder viewed (a playing with
`--perspective` of the recorder's own record); in full view, or from another
seat, the views differ and the contact bits are not compared. The count of
units compared without them is on every summary line, and the owner approved
that rule for dumps on 2026-10-06.

MEASURED 2026-10-06: before the dump path took the rule, every multiplayer dump
had been taken in full view or from a seat the recorder did not view, so none
reached it. From the recorder's own seat, the four remote humans' commanders of
the five-peer ten-seat recording differed in bits 8-9 at 300 and 1000 (their
owners' machines view their own players; the playback's radar pass `0x467440`
computes the recorder's view, as the recorder's own scan did), and the two-peer
recording differed in 18, 11, 10 and 9 units at 300, 600, 900 and 1200. With
the rule all are EQUAL with 0 units unscanned, and a contact bit cleared in one
dumped unit is found as exactly one difference.

The residual: the expectation is the recorder's scan, but the host's radar pass
reads the composed world -- each remote unit at its owner's image position --
while the recorder's read its replicas, which in the two-peer recording sat up
to 10.3 world units away (1 to 6 remote units per sampled tick). A unit or a
jammer straddling a sight, radar or jammer edge at a pass would differ with no
playback bug. None was seen in six dumps; the owner chose to keep the
comparison strict and investigate one if it appears.

MEASURED 2026-10-06 on the reference setup, internal speed 20, the
reproduction recording:

- **At 1000 as player 1**, the remote human seat: 36 units made, 32 order
  nodes built and 1 adopted. The fork's first iteration is EQUAL to the
  composed parent. The live game's dump at 1120 is EQUAL to timeline 1's
  recording, and `tarec check --timeline 1` explains every change.
- **At 246 as player 0**, the recorder's own seat, inside an owner-first
  window: 65 units, the fork EQUAL, the live dump at 366 EQUAL.
- **The AI plays** (tareplay's takeover check, a 1,800-tick window after a
  pause). At 1000: seats 0, 2 and 3 issue 80, 39 and 20 orders, 96% of
  their 25 mobile units move, and seats 0 and 3 build. At 246: seats 1, 2 and
  3 issue 33, 39 and 12 orders, 96% of 28 mobile units move, and all three
  build.
- **Timeline 1 played back** across its fork: in one playing, seeks from 920
  to 1100 (into the timeline) and from 1166 to 900 (back into the
  multiplayer segment), and the boundary's own crossing at 1000. Dumps are
  EQUAL at 900 (twice), 1020, 1100, 1120 and 1140, the multiplayer ones by
  `recmp_compare.py` and the timeline's by `tarec compare --puppet
  --timeline 1`.

- **A running playback taken over** at 900 as player 1, after it had played
  and composed to 300 on its own: the fork EQUAL, the live dump at 1020 EQUAL,
  and the AI seats issue 68, 27 and 18 orders.
- The skirmish `tareplay.py takeover battle --cover` passes every check on
  the same build, compare-fork EQUAL.

**Not run:** a takeover inside the timeline itself, and a running playback
taken over backwards (the driver arms its dump before the takeover, and the
recorder refuses a dump at a GameTime the level has passed).

### Each player's explored map in playback

The explored map holds one bit per player in every entry, and the recorder's
copy of a remote player's bits is what its own engine stamped from replicas,
not what that player explored. Each owner contributes its own bit
([*Per-origin explored-map history*](#per-origin-explored-map-history)), so
the materializer composes the map as it composes units and players:
`mp_authority` reads each actor's bitmap from the reader's history at the
reader's tick, the authority its player record uses, and `mp_world` writes
the actor's bit in every entry from it at the record's `+0x146`, the bit the
recording machine stamped. The other bits are the recorder's, and a player no
owner has described yet keeps them. `recorded_put` writes the composed map in
every step and seek, so a selected player's view, the takeover's live game and
the dumps all show the owner's exploration. `recmp_compare.py` composes the
expected map the same way.

MEASURED 2026-10-06 on the reproduction recording: the owners' and the
recorder's bits differ in one entry, row 3's at entry 10833 from GameTime 244,
which its owner explored before the recorder saw it. The dumps at 244, 246,
364, 900 and 1958 are EQUAL with the composition, both viewed from the default
seat and from record 3, and without it the comparison names that entry. The
recording's units barely travel, so a recording with long moves is still to
measure (the exploration research measured 34 cells for one moved commander).

### Stage 4: perspectives and communication

Owner decisions, 2026-10-06: 4a, each player's explored map (above); 4b,
chat played back with its audiences, every message in full-map mode and only
what a player received in that player's view, with a hide-chat toggle; 4c,
every player's camera, cursor, clicks and selections recorded and shown as
overlays, in skirmish recordings too (the human seat). Pings and drawings are
dropped until Impure has such a feature: stock TA has none [INFERRED: no ping
or marker message found; TADR's markers are not ported], and a future one
follows chat's audience rules.

4c is its own plan, [demo-recorder-perspective-streams.md](demo-recorder-perspective-streams.md).
Its skirmish landings 4c-1 to 4c-3 record the human seat in the primary stream
and play it as Follow, Jump and the overlays. Landing 4c-4 brings them to a
multiplayer recording as three contribution kinds, built 2026-10-06 and not yet
run between two peers ([4c-4 as built](demo-recorder-perspective-streams.md#4c-4-as-built)):

| Kind | Name | Body | Sent |
|---|---|---|---|
| 13 | `RECMP_POV` | one `RecPov`, 32 bytes: view, applied zoom, camera centre, cursor | at most every 6 ticks (5 Hz), when it changed and the transmit queue has 64 free slots |
| 14 | `RECMP_CLICK` | one `RecClick`, 20 bytes | from the click redirects, at the channel's capture tick |
| 15 | `RECMP_SELECT` | a `RecSel` head and at most 670 `(slot, birth)` entries | the whole selection, BEGIN to END, when it changed other than by a death |

Every payload's player byte is 0xFF, since the origin names the actor. A
selection's slots lie in the actor's roster block. A seal is refused while a
selection transaction is partial, as for kinds 2 and 11. Wire version 2 is
unchanged, and a peer without kinds 13-15 refuses them, so every contributing
peer needs the same build. The playback reads them at the application tick:
Follow and the cursors interpolate between the samples' application ticks, a
click shows from its envelope's application tick, and a selection is held to
the composed lifecycle's birth.

### Chat in playback

*THE CHAT OF A MULTIPLAYER RECORDING* in `tagpu_replay.c`. Each text entry
([*Persisted text identities and receipt clocks*](#persisted-text-identities-and-receipt-clocks))
is shown when the recorder applied it, the clock of every composed
contribution, through the stock display sink `0x463CA0(text, flags, unit,
source row)` that the text receive case calls. The viewed record picks the
audience:

- **The full map** (the viewer's own record V): every message once, at its
  author's original, with the author's row and flags 8.
- **A player's view**: what that player's machine showed. Its own lines show as
  the formatter's local echo (source row 10, the entry's display flags, as
  `0x463ED4` passes), and the lines its machine received show with the author's
  row and flags 8. An AI seat receives no text (the receive case requires a
  controller-1 destination), so its view shows none.

A seek shows no line before T: `sk_deadlines` empties the message log through
the level init's reset `0x463C80`, as before. `chat hide` and `chat show` in the
trigger file toggle the display, and the playing's last line counts the text
entries applied and the lines shown. The toggle has no control on the screen
yet.

The message log is a `leave` row, so `recmp_compare.py --chat VIEW[:SINCE]`
checks it apart from the comparison. The log's chat lines must be the newest
expected ones, in order, each stamped with the GameTime of the step that applied
it (its application tick or the one before, THE CLOCK). Every older expected
line must have left the ring, which drops only its oldest line. Two things drop
it: the step's expiry `0x463EF0`, once the line's stamp plus (`main+0x37F23` +
1) × 30 ticks is below GameTime (DISASSEMBLED, called at `0x495683`), and the
draw `0x448CFA..0x448D2C`, while more lines are queued than the panel holds. A
seat's own notices fill the panel too.

MEASURED 2026-10-06 on the reference setup: a two-human, two-AI game on *Two
Continents* in which each human says a public line and sends the other a
private line (`recmp_create_begin_test.py --chat`, eight entries: four
originals, four receipts). The playing applies all eight in every view. The
full map shows the four originals: three at 600, none at 1400 once they have
expired (a line lives 330 ticks at the option's 10), and after a seek from 1400
to 500, two at 620. Each human's view shows its two own lines as echoes and
the two lines it received: at 700, player 0 still shows one and player 1 none,
since player 1's "Under Attack" notices pushed the rest out of the panel. AI seat 2
shows none, and `chat hide` shows none in the full map. Every dump passes, and
each dump checked against another view's expectation fails.
The skirmish `secondary-orders --cover` regression passes all 25 checks on the
build carrying 4a and 4b.

Stage 5 seats the viewer in player record 10 on every playback, so a recording
that seats all ten records plays ([the ten-seat note](demo-recorder-ten-seats.md)).
Still open: the traffic budget, migration and cross-address discovery, in that
order.

A player who leaves the game is a lifecycle landing of its own (*Named
residuals*).

### Fixed global owner blocks before construction

Offline skirmish allocation sorts by player-record address; native multiplayer
sorts by native ID. Setting an offline player's ID therefore cannot preserve the
recorded global unit blocks. Reconstructing a saved owner unit requires that
its recorded slot already belongs to that owner's normalized player row.

The allocator observer at `0x48587E` supplies this invariant before any unit
constructor. It runs after the native sort and before the paired block initializer.
`tagpu_recmp_arena_order` validates the frozen roster and all ten copied pointer
values as an exact permutation of the ten player records, then replaces only
that stack table. Active rows retain their recorded global ranks; vacant rows
and ranks pair in ascending order. All ten seats are supported without reserving
a row. No pointer is followed by the copied helper, and refusal changes no
table byte. The native initializer subsequently writes player first/last pointers
and IDs, unit owner pointers/rows and initial groups together. This avoids moving
live units or manually changing one side of those paired fields. The loader
thread owns the fresh arena and stack throughout that initialization. Seating
publishes an immutable copied roster before that thread starts; the allocator
claims it and publishes READY/FAILED with interlocked operations. The first
game-thread step, after the engine's load-done gate, requires READY. The loader
does not touch the mutable reader or game-thread failure flag. Plan reset occurs
before thread creation on Start undo or after loader completion at teardown.

MEASURED 2026-10-05 on the reference setup:
`python3 tools/replay/recmp_arena_test.py --out <directory>` passes 640 copied
permutations, overlap and twelve unchanged-output refusals. A standalone fixture
executes the relocated retail allocator and its real sorting functions, replacing
only its three heap calls and retargeting 24 verified absolute operands. It
checks two untouched offline/native-ID ordering controls and 206 recorded layouts:
one through ten actors, noncontiguous blocks, every unit's paired owner fields,
player block pointers/IDs, group -1, original global slot IDs and the sentinel.
Format-bound cases include each=1 and 6,553; these do not expand the game's
permitted unit limit. The first two fixture attempts could not reserve the retail
address range because Wine's process heap owned it. Relocating the fixture's
private code/data image resolves that conflict without releasing another mapping
or changing the original executable.

Normal and coverage builds pass. The production observer is installed on all
four `secondary-orders` coverage processes and leaves their skirmish layout
unchanged: all 23 checks pass at internal speed 20, complete tick 1,140,
1,139 transitions explained and three exact poisoned round trips at 361/481/511,
each with 26 tables and zero residue. No exclusion or tolerance changes.

The loader handoff correction's coverage regression passes 22 of 23
`secondary-orders` checks at internal speed 20: complete tick 1,110 and 1,109
explained transitions. Recorded/puppet/restored/fresh seek comparisons pass,
and round trips 421/451/541 poison 26 tables with zero residue and exact
before/after dumps. The images inventory reports `main+0x531`, one byte of
the desktop GUI pointer, as written during play only on the recording machine.
That byte is the `leave` row `guitop` since 2026-10-06. This run exercises
the installed callback's inert skirmish path and seating's OFF publication;
it does not exercise a multiplayer loader handoff or clear the complete
landing gate.

The standalone allocator fixture proves the paired initialization, and the game
regression proves the installed hook remains inert on skirmishes. The
multiplayer loader route is exercised by the [materializer's](#owner-authority-materializer)
saved playing. Ten-row allocation support does not implement ten-seat replay
hosting or its viewer policy.

### Owner destructor entry and return verification

MEASURED 2026-10-05 on the reference setup: current coverage code runs a native
*Two Continents* combat/reclaim game, two humans and one hosted AI each,
1,500-unit actor blocks, host recording on and joiner off. All 24 first
create/death/return/script/reply/spray observations match source and recipient
batch bytes, CRC, source tick/sequence, call index, birth and phase. The complete
2,432,312-byte host file spans GameTimes 1..468 with 468 iterations and zero
corrections. Its ordered ledger retains 5,994 applications, 2,632 consumed.
C and independent Python retain the complete application sequence and 36 primary
replica-create roots through fresh world seek: application bytes 1,546,794,
1,054 root batches, CRC `5C4F39B4`; primary creates CRC `9B62B4A7`.

The ledger contains 22 matched destructor entry/return pairs with identical
origin, slot, birth, source tick and capture phase. Host human/AI contribute
4/1 deaths; join human/AI contribute 12/5, with recording off on that peer.
All death pairs in this sample are post-B2. A join-human named start at source
tick 109, sequence 271, unit ID 18/birth 42 is pre-B2; its publisher and recipient
bytes and phase match. This native sample therefore observes both root phases,
but does not establish pre-B2 death coverage.
Across the 3,699 applied owner calls, 2,166 are post-B2 and 1,533 pre-B2.
All five locally owned primary death payloads match their mandatory entry bytes
at the same source tick. The observed world has 23 primary deaths versus the
ledger's 22 owner deaths; these remain separate namespaces, and this capture
test establishes no rule for reconciling them in a canonical replay world.

The retained DLL/map and bitmaps show 678/549 reached functions. Both peers
reach `ev_death`, `death_room`, `death_enter`, `death_leave`, `mp_roots_prepare`,
`mp_roots_publish`, `mp_roots_push`, `mp_roots_log` and `mp_capture`. Root-cache
code outside the instrumented source set is exercised by its separate fixture;
no coverage reach is claimed for it. The host ends with 3,362 accepted / 3,368
completed packets, queue/unread/failure zero, cuts 2/2 and four receipts. The
joiner later ends with 2,756 / 2,762, queue/failure zero, three unread packets,
final sources 2/2, cuts 2/2 and four receipts sent. The joiner is on
`ARMMAIN2.GUI` after host leave, so this verifies a frozen recording cut and
ordinary exit, not continued migration. Native speed remains 10. All owned
games/prefixes/display are removed. No engine patch/write, exclusion, tolerance
or timing mitigation is added; native playback/seek/takeover and the full
multiplayer landing gates remain open.

The same source passes all 23 coverage `secondary-orders` skirmish checks at
internal speed 20: live/puppet/restore/fresh seek, event/image comparisons,
inventory, address scans and puppet writes. Round trips at 391/481/511 poison
26 tables, leave zero residue and compare exactly before/after. The 39 harness
logic tests pass with refreshed coverage maps; their initial stale-map failure
named the new helper and was resolved by building the current coverage map.
No comparison rule or exclusion changes. This is a verified worktree checkpoint,
not the accumulated replay landing gate or a multiplayer landing.

### Native contribution datagram probe

The standalone production-channel fixture accepts `--body-bytes 1..4032` and
keeps the default 16-byte body. The C fixture checks every copied DATA byte,
source sequence, byte accounting, human/AI prefix seal and terminal receipt.
Its independent Python journal checker retains its CRC, sequence and final-prefix
refusals; the exact journal span follows the selected body size. Neither path
changes the production channel or an engine field.

`--trace-network` uses strace on only the two owned native peers and their
descendants. It records successful network calls with timestamps, decoded socket
endpoints and complete hexadecimal buffers up to the configured capture limit.
The runner closes the owned prefixes, observes peer termination and waits for
the associated tracers to end before returning their evidence. Setup, control
fixtures and other games are outside that trace scope.

MEASURED 2026-10-05 on the reference setup: the maximum-body production-channel
run passes both peers' sixteen DATA deliveries after primary Close, final
prefixes, receipts, byte accounting, persisted-journal round trips and corruption/
missing-tail controls. Each journal has 40 applications and 132,528 bytes.
The successful UDP `sendmsg` calls show the following DATA spans on each peer:

| Application DATA | UDP datagrams per message | UDP payload bytes per message | Total for sixteen messages |
|---|---|---|---|
| 4,096 bytes | 1,394 + 1,394 + 1,334 bytes | 4,122 bytes | 65,536 application / 65,952 UDP payload bytes |

An independent byte audit groups the observed fragments, removes their measured
14/6/6-byte prefixes and reconstructs all sixteen complete application packets.
Their byte multiset equals the local-produced DATA in each peer's journal.
Fragments can interleave; this comparison establishes no cross-source ordering
rule. At the default body size, each identifiable 80-byte application DATA in
the earlier probe occupies one 94-byte UDP payload. Both probes contain separate
14-byte datagrams without an application marker and repeated identity images.
Counting only datagrams that contain `IMPUREMP` therefore misses continuation
fragments and unmarked traffic.
A TCP byte counter also misses this observed UDP data path.

These are kernel-accepted UDP payload observations. IP/UDP/link headers, kernel
TCP retransmissions, socket lifetime attribution of unmarked traffic, a shared
bottleneck, cross-address discovery and game-traffic delay are not established.
The reference setup permits tracing owned processes but denies packet capture
and an unprivileged network namespace. No privileged network change is made.
The physical budget and no-game-delay gate remains open, as do native engine
playback/seek/takeover, complete owner authority, POV/chat, ten-seat play,
continued migration and the accumulated review/replay landing gates. This is a
measurement fixture checkpoint, not a multiplayer landing.

### Per-origin explored-map history

The primary stream's raw mapped map describes the recorder's engine copy. It
cannot supply a remote controlling peer's actual exploration history. Each
local human/hosted AI now contributes its own source-row bit from every engine
mapped-map u16, packed in the original linear cell order. Native origin identity
keys the bitmap; player-row permutations move neither its cells nor its bytes.
A validated source roster still binds the origin and stream before any chunk.
The [engine map](../exe-reverse-engineering.md#the-demo-recorders-globals-projectiles-and-seek-disassembled-measured-2026-10-01)
records the allocation, bit writer and Mapping handler disassembly.

`RECMP_EXPLORED` (kind 11) uses a packed 16-byte prefix: u32 first/total byte
offsets, u16 count/LosType, BEGIN/END/FULL flags and three zero reserved bytes,
then at most 4,016 bitmap bytes. The first image is full; later ordered byte runs
preserve both set and cleared bits, bridging up to eight equal bytes. An
unchanged bitmap emits nothing unless its full 16-bit LosType changed, in which
case an empty BEGIN/END transaction retains the mode. Housekeeping bit 3 is
retained. Wire version 2 and file formats 5/6 are unchanged; a decoder without
kind 11 refuses this new data, so all contributing peers require the same build.

`tagpu_recmp_explored.c` owns copied buffers on one thread. Dimensions must fit
LoadMap's exact whole-u16 allocation without 32-bit multiplication/shift wrap.
Each complete copied chunk validates length, flags, reserved/tail bits, range,
source roster/stream, tick, sequence and transaction ordering before any write.
A full transaction has contiguous coverage; deltas require a complete baseline
and ascending nonoverlapping runs. Only END publishes. Source seals require no
partial transaction. Encoder input is copied before callbacks; failed emission
ends the producer and cannot restart its partial transaction.

`mp_explored_capture` reads only controller-1/2 rows on the game thread at B2,
including when local recording is off. Frozen pointer/geometry checks detect
level replacement; allocation-to-teardown game-thread ordering supplies the
lifetime. File history separately validates source proof and publishes applied
exploration. `recr_mp_explored` returns the last complete bitmap, full LosType
and source/application provenance at the current world tick, rebuilding after
seek. Unknown and insufficient-capacity queries leave outputs unchanged. No
engine write, byte patch, exclusion, tolerance or simulation class is changed.

The owned Wine codec fixture passes ten shuffled origins, input destruction
during callback, split full/delta publication, unchanged and mode-only updates,
53 malformed/guarded-span controls, unused tail bits and failed emission. The
writer/C/Python file fixture checks applied full/delta/mode data, seek, proof-only
isolation and semantic refusals behind resealed valid source prefixes/cuts.
These are copied-data tests. They do not establish effective POV playback,
camera/cursor/selection/click streams, recomputed sight or communication audiences;
native engine playback and the wider multiplayer gates remain open.

MEASURED 2026-10-05 on the reference setup, COVER build, retail *Two Continents*,
private display and native loopback DirectPlay, host recording enabled and joiner
opted out: all four complete first bitmaps have source tick 1, sequence 17,
134,400 cells, 16,800 bytes and LosType 7. Source/recipient CRCs agree exactly:

| Origin | Controller | CRC | Recipient application tick |
|---|---|---|---|
| 666445500 | host human | `9D86AD27` | 14 |
| 666445497 | host AI | `AFB762D4` | 14 |
| 666445502 | join human | `7134ABF3` | 57 |
| 666445496 | join AI | `E3D77AE4` | 57 |

The same run verifies the four initial rosters/player images and all 24 first
create/death/return/script/reply/spray batches. It then fails the ordinary-exit
driver: batched chat input leaves `TALK.GUI` on top instead of `TABMENU`.
The next attempt stops before the game because its temporary nickname exceeds
the engine's ten-character field. The third uses short names and waits for
`TALK.GUI`, but `ui fill TALK +mapping` returns an empty field rather than the
requested text; it refuses to submit unconfirmed input. All owned instances
and private displays are removed. This native gate stops after three attempts.
No complete native exploration file, real Mapping change, primary-map oracle,
C/Python native exploration seek result or physical budget is claimed. The next
native attempt needs a driver that demonstrates acknowledged chat text or
another engine-owned mode-change control. These failures leave the successful
initial exchange intact and do not establish a simulation or transport defect.

The current controlled file is 4,018 bytes: 30 applications (20 produced, ten
consumed) and eight proof records. Six mixed owner root batches keep their exact
order/bytes; the new local exploration full/delta/mode images are applied while
a later remote full image stays proof-only. All five lifecycle checks, 23 header
controls and 120 semantic-body controls pass, including eleven exploration
mutations whose rebuilt raw seals/cuts are valid. The first expanded proof-only
test omitted the original late proof packets from its constructed proof stream;
including the complete source prefix fixes that test, without decoder changes.

The current COVER `secondary-orders` run at internal speed 20 returns 22 passes
and one failure: the images inventory finds three differing bytes at
`main+0x531`, live `48c60b`, playing `70030c`, fresh seek `40050c`. Observed world,
radar, roots/sinks, address scans, puppet-write checks and all three exact
poisoned round trips pass. Those bytes are the `leave` row `guitop` since
2026-10-06, with which the same fixture passes all 25 checks.
The normal/COVER builds and all 39 harness logic tests pass. The native bitmap
coverage sample reaches `mp_explored_begin`, `mp_explored_capture`,
`mp_explored_emit` and `mp_explored_log` on both peers; the sample is written
before teardown and does not reach `mp_explored_end`. The codec's private copied
storage is outside the instrumented recorder source set. No successful complete
native exploration replay or accumulated landing coverage is claimed.

The updated C/Python readers also verify the earlier real native file: all
5,994 applications, 1,546,794 application bytes, 1,054 root batches and 36
observed replica creates remain exact through world seek. That file contains
no kind-11 exploration; both readers retain unknown exploration rather than
inventing a baseline. This is backward file-reading verification, not native
exploration application or engine multiplayer playback.

### Text-chat boundary and audience audit

The communication contract includes in-match public/team/private text, pings
and drawings, with all messages in full-map mode and only received messages in
a selected player's view. Lobby text is excluded. The work branch carries
identified text sources and actual recipient observations as kind 12; display,
loading acceptance and the other communication forms remain open.

DISASSEMBLED 2026-10-05: the stock text sender formats a line through
`0x463E50`, builds a fixed 65-byte `0x05` message through `0x453360`, and
chooses targets before transmission. Mode 0 uses the generic sender, modes
1/2 use the controlled player's alliance vector, and mode 3 uses ten explicit
recipient bytes. The targeted sender `0x451BC0` applies additional source and
destination filters; even a successful return can be a local shortcut without
a transport call. It therefore does not establish a received audience.

The dispatcher maps native source and destination IDs from `main+0x4C9/+0x4CD`
to records before the text case. That case displays text only for an active
controller-1 destination. The display queue holds at most 63 visible bytes per
line, with source row and GameTime; its shared sink also receives unit replies,
console replies and notices. Capturing every display-sink call as human chat
would invent chat events. Capturing only a recorder's own receive path would
lose private/team text it was never addressed. Names in formatted text are
not identities, and identical repeated lines are separate events.

The engine map's *In-match text chat* section records the call sites, argument
contracts, field offsets, source/destination lookup, local shortcut, display
truncation and the existing B4/B8 hook ownership. Each primary receive delivers
one split message into its current buffer; B3 retains that exact delivered span.
The identified text receiver below uses this bound before copying.

The native text wire now carries a unique sender event identity as described
below. Intended targets and actual accepted recipient observations still need
playback, preserving their separate
source/application times. Original and forwarded copies must join one event
without deduplicating text or inferring a recipient from a transport return.
Primary per-pair ordering is still unproved; the same-wire identity removes
the need to correlate these observations by ordinal. Pings/drawings still need
their own boundary/audience audit. This does not close the POV/chat gate or
implement the chosen owner-lifetime policy.

#### Identity travels with the native chat

The work branch registers a 97-byte primary code `0x2B`, with version/length,
fresh match GUID, a nonzero 64-bit sender serial, source GameTime and the original
65-byte stock text packet. `(match, native sender, serial)` names one event.
One scope at the in-match GUI call encloses the native formatter, audience loops,
every destination send and local echo. Other front-end calls and skirmish remain
stock. The serial never wraps. The three native send sites wrap that same event
before calling their original sender; they preserve native audience/filter logic
and keep its success distinct from recipient acceptance.

The receiver shows every well-formed envelope and records only what it can
name. An envelope of the exact 97-byte span with the code, version, length and an
ordinary text packet (`0x05`) has that original packet copied to the front of the
owned receive allocation and enters the original text case `0x45522E`, which
checks its own destination as it does for stock text; a malformed one is dropped
(`0x455F50`). Recording it is separate: it requires a network level in state 5
or 6, both native actor rows bounded with their IDs, a controller-3 sender and a
controller-1 destination, and the inbox's match and serial checks. A line refused
there is shown as the stock game shows it, logged as "shown and not recorded",
and not recorded: whether a line appears never depends on the recorder. MEASURED
2026-10-07: a two-peer game with four lines records and plays them EQUAL with
no line refused; in a three-peer game whose host left first, each joiner's
engine cleared every other remote row (rows 1-3 to ID 0xFFFFFFFF), so a later
line from one joiner reached the other on neither this build nor the previous
one -- the engine had no other player left to address, and the refusal this
separation removes was not what kept it off the screen. The receiver neither changes the pump pointer nor
retains an engine address. Code `0x2B` has a fixed splitter length, a default
callback with the same body as stock text and stock text's receive mask (7). All sites are acquired before any patch is written. This same-build wire
extension adds 32 application bytes per transmitted human text message; this is
arithmetic, not measured physical traffic or an approved bandwidth result.

Normal and COVER builds pass. Copied codec controls pass repeated full-length
text with different IDs, maximum serial, source/destination aliasing, 162
guard-page-truncated spans and six malformed headers/packets. Every refusal
retains the output unchanged. The source and recipient logs retain identity,
text-packet CRC and separate clocks; they are diagnostics, not persisted
communication-ledger records. No complete communication file, playback, seek,
pings/drawings or native raw-transport result is claimed. Loading-phase chat
delivery is also unverified by that probe: its receiver required the live B2
producer and state-6 dispatch. The first native wire probe sends the stock path
because it incorrectly required the pending start flag that B2 has already
consumed; the active producer's owning-thread lifetime replaces that check.
That run then stops on a failed long-line input acknowledgement, before its
wire-ID assertions. It is not a passing native identity result.

The corrected COVER run passes in-play queued native delivery with three
humans and local recording off. Five source identities `1..5` preserve exact
65-byte packet CRCs and source ticks on the selected recipient; the other peer
accepts `1,2,4,5`. The two identical public lines have separate identities with
the same CRC. Event 3 is the digit-selected private line. Event 4 fills the
64-byte wire text and has exactly equal 63-character displayed text on every
peer. The source tick 697 is received at 662 and 660 respectively, preserving
both clocks. Event 5 is the subsequent public marker. The first long-line
attempt did not acknowledge a wide-character field replacement; the corrected
run uses a 60-character narrow-letter line and acknowledges it exactly.
No cause for every long-line driver refusal is claimed. The engine map lists
the complete source/recipient ID/tick/CRC sequences. The tested DLL/map/hash,
logs and queue snapshots are retained, and all owned instances/prefixes/display
are removed. This remains a wire checkpoint with the wider gaps above.

#### Persisted text identities and receipt clocks

Kind 12 batches carry copied 136-byte entries: source/received phase, audience
mode, native source and recipient IDs, nonzero event serial, original source
tick, display flags, up to ten intended target-call IDs and all 65 stock bytes.
The envelope's tick belongs to its producer: the author for a source entry,
the actual recipient for a received entry. The journal separately retains this
recorder's application tick and ordinal. Source entries include the formatter's
local echo, copied after its unconditional local sink returns; received entries
capture the validated native receive boundary. Public mode keeps the generic
audience distinct from a list of targeted calls. Received entries have unknown
intent and never infer another recipient from a successful send return.

The bounded identity cache joins `(native author, serial)` within the match,
including receipts arriving before originals. It checks all packet bytes and
the original tick; targeted originals must include every claimed receiver.
Source serials increase; actual repeated/reordered receipts remain separate
ordered observations. The 65,536-identity bound and 4,096-entry copied queue
refuse overflow rather than drop an event. Every batch validates before any
cache publication. Root and text queues drain their earliest copied tick first,
before advancing the shared source clock; teardown uses the same ordering.

Writer and C/Python reader source histories validate even proof-only entries.
A complete footer requires an original for every claimed receipt. Separate
application history excludes proof tails; `recr_mp_text` is a latest-batch
inspection query, while the application visitor retains every original and
receipt through seek. These queries do not display multiplayer replay chat.
The native loading boundary, raw transport, other communication forms, complete
POV reconstruction and engine playback remain open.

Copied controls pass ten shuffled origins, full-length/repeated text,
receipt-before-original and repeated/reordered receipts, private intent
validation, 28 malformed batches, 412 guard-page truncations, the identity
bound and queue wrap/overflow/failed-publication refusal. C/Python serialized
history controls reject a sealed but absent original, join a proof-only
original, preserve separate clocks through prefix reconstruction and keep
that proof-only original out of application history. The existing full
`.imprec` writer/reader/failure regression suite also passes.

The live COVER storage probe passes with three humans, one saving and two
opted out. Source `676590527` emits IDs `1..5` at ticks
`273,384,511,691,821`. Recipient `676590525` observes all five at
`238,349,476,656,785`; `676590523` observes `1,2,4,5` at
`236,347,654,784`. The host records those remote observations at
`280,391,519,699,828` (the other peer has no private event at 519).
The source's intended target for private event 3 is exactly `676590525`.
Two identical public lines remain distinct IDs. All 65 bytes/CRCs agree between
the wire's source/receipt logs and the fourteen persisted observations. Full
text remains 64 wire bytes with equal 63-character native displays. Ordinary
departure through each peer's own menus closes the recording at GameTime 1027
with 1,027 iterations, 429 application envelopes and zero corrections. Python
strict source/end validation accepts the complete file. The C reader recovers
the same fourteen complete entries as Python, including exact packet bytes,
intent and all three clocks; wire source/received metadata matches the file.
Sequential reading and reader world seek both recover all 429 envelopes /
174,679 application bytes, CRC `2E8EA472`, from GameTime 1 through 1027.
The C latest-batch queries and Python prefixes also agree in body bytes and
origin/stream/sequence/source/application provenance before and after reader
seek to the middle and end. Repeated queries do not acknowledge application
visitor records.
This idle chat fixture has zero owned effect batches. The combat-native reader
fixture correctly refuses that missing prerequisite; its existing effect
requirement is retained, and the explicit `native-text` reader control requires
text observations instead. This does not complete the owner-effect gate or
enable engine playback. This run does not provide an independent raw primary
exploration oracle or the accumulated landing coverage.

MEASURED 2026-10-05 on the reference setup: a separate native three-human
*Town & Country* COVER game explicitly arms local saving off on both private
authors. Each sends one digit-selected message to the other; the saving host
is addressed only by its own subsequent public marker. After that marker the
host's native display queue contains neither private line, and its wire log
has no private receipt. The complete 428,123-byte recording nevertheless
retains both private originals and their actual receipts:

| Author / intended destination | Serial / stock CRC | Original source / recorder application tick | Actual recipient / recorder application tick |
|---|---|---|---|
| `677804643` / `677804645` | 1 / `FCC570F5` | 265 / 311 | 262 / 313 |
| `677804645` / `677804643` | 1 / `FDABD2F3` | 355 / 406 | 366 / 411 |
| `677804641` / generic public | 1 / `F70E8277` | 515 / 515 | `677804643`: 478 / 522; `677804645`: 471 / 522 |

All three author/serial pairs remain distinct. Private originals have mode 3
and exactly the other's native ID as intended target. The two opted-out
authors create no writer according to their retained logs. Ordinary departure
closes at GameTime 728 with 728 iterations and zero corrections. C/Python
recover all seven exact entries and the complete 324-envelope / 160,587-byte
application history, CRC `DDA07B57`, through reader world seek. Eight C/Python
latest-batch queries agree; the host has no batch at the middle seek because
its first source event is later. Unknown is retained rather than invented.
This idle chat recording has no owner effect batches or engine playback result.

`recmp_file_test.py --native-text-file FILE --chat-evidence DIR` is the
repeatable saved-evidence check. It requires text observations and a complete
footer, compares every C/Python entry and latest-batch query through seek, and
checks original/actual receipt identities, CRCs and clocks against each peer's
retained log and native roster. The existing `--native-file` combat and primary
exploration oracle requirements remain intact. The repeated/full-length
fourteen-entry recording and this three-author seven-entry recording both
pass the saved-evidence check. These are reader tests, not engine replay
application or the accumulated multiplayer landing gate.

The loading audit identifies the mask gap: stock text mask 7 admits state 5,
whereas the identified path's initial mask 4 admitted only state 6. The loader's early
pump and the game loading frame can both receive. State 5 also persists into
the first catch-up ticks after the loader's final completion store, before B2
creates the contribution producer. B3's TLS span identifies a receive; it
does not serialize loading pumps or protect their shared reallocating buffer.
The [engine map](../exe-reverse-engineering.md) records the call sweep, state
mask and loader ordering. Loading receipt ownership/clocks and stock display
require the copied handoff below; an actual native loading receipt and its
saved clock/order remain unverified.

#### Primary input ownership before loading admission

The work branch replaces the pump's shared incoming allocation with one
process-lifetime allocation per recursion depth, reused across calls, threads
and levels. The entry wrapper at `0x453D40` keeps a recursive
critical section through receive and native dispatch; busy admission returns 0
without waiting, changing TLS/IDs or consuming an SDK message. The receive-call
redirect at `0x453D94` copies from the original transport/SDK into that frame's
storage and assigns the pump's existing stack message pointer only on success.
Native sends keep using `main+0x2A38` and cannot overwrite this input. Nested
pumps own distinct allocations, restore their parent's exact span and
`main+0x4C9/+0x4CD` IDs, then resume it. The full u16 allocation bounds the
transport's unchecked queued-message copy. Raw/system receives negotiate
larger allocations before exposing any span. All thirteen primary sites and
the splitter's length-bound instructions are checked before any write.
No engine interface is retained. The last packet remains allocated after pump
return until a subsequent receive at that depth overwrites or resizes it.
Nested receives use a different depth slot. This introduces no earlier native
alias invalidation than retail's shared buffer, which any receive can reuse.

The [engine map](../exe-reverse-engineering.md#owned-primary-receive-frames)
records the native receive contracts, queued-copy exception, system size
negotiation, two stock statistics calls, direct caller/reference sweeps and
the case that builds an outgoing reply in the shared buffer. Input ownership
does not establish safe lifetimes for every separate native output builder.

`recmp_receive_test.py` compiles the exact production receive core and the
actual receive-span API and movement-copy integration, with only the
SDK/statistics targets redirected to controlled fixtures. It passes the
65,535-byte queued limit, raw growth to 70,001 bytes, output-buffer overwrite,
nested storage/span/ID restoration, invalid-size/error/network refusal and
4,000 two-thread receives with 200 nested calls. Parent and nested aliases
survive return, both depth buffers are reused, and a foreign thread finishes
busy admission while the owner holds the lock without consuming a message.
It also verifies movement
header clamping, zero padding, legacy fallback, and interior/inactive-parent
refusals. Its prefix/display are removed.
The generated entry/call wrapper is exercised separately by the native game.

MEASURED 2026-10-05 on the reference setup: three humans, *Town & Country*,
COVER, native queued DirectPlay, internal speed 10, only the host saving.
Each saving-off author sends serial 1 privately to the other; the host sends
a public marker. The host's sampled display has neither private line and its
log has no private receipt. IDs `680809199`/`680809193` retain stock CRCs
`5C1DCD80`/`5D736F86`, original ticks 245/352 and actual acceptance ticks
254/354. Host `680809197` sends CRC `BD3EAEC2` at 499, accepted by the other
peers at 461/465. The file closes with 709 iterations and zero corrections.
C/Python agree on all seven entries, eight latest-batch queries and the full
313-envelope / 159,095-byte application history, CRC `CEA69AB6`, through reader
seek. Both opted-out logs lack a writer. Their contributions refuse after the
host leaves, as in the preceding baseline probe; continued host migration is
still open. The first raw follow-up also delivers these audiences and closes
its host writer, but its menu driver fails on `ENDMSN`, so no complete raw
saved-file/read gate is claimed from that attempt. A second raw run fixes only
that exit driver and completes at tick 799 with zero corrections. Authors
`681831592`/`681831598` send private CRCs `98F0C09C`/`999E629A` at ticks
267/435, accepted at 339/364. Host `681831594` sends public CRC `97588B0B`
at 590, accepted at 473/545. The host displays neither private line. C/Python
agree on seven entries, eight latest-batch queries and all 340 application
envelopes / 162,631 bytes, CRC `0E49B035`, through reader seek. The observed
native speed is 10 despite requesting registry speed 20.

Native movement testing then exposes an integration regression: the guarded
`0x2C` decoder still consults only the retail receive TLS. Both owners move
locally, but remote images stay at their initial coordinates and all movement
packets are refused as stale. The preceding branch baseline propagates them.
The decoder now uses the same exact-span API as identified chat. The fixed
queued and raw two-human fixtures observe both commanders move on both peers,
positive movement packet counts and no stale refusals. Queued finishes at
ticks 363/342 with 333 movement packets per endpoint; raw finishes at 331/299
with 294/245. These are native live receive
checks, separate from the stopped independent primary exploration oracle.

The persistent-depth/nonblocking follow-up passes both native fixtures on the
same retained COVER build: queued ticks 315/295, movement counts 270/319;
raw ticks 336/311, counts 200/222. Every sampled stale count is zero and neither
endpoint reports contribution refusal. This executes the installed 32-byte
wrapper, with saving off, and removes only its own instances/display. It does
not test saved-file playback, loading chat or physical bandwidth.

Incoming storage separation, retained packet lifetime and nonblocking pump
admission are implemented and tested. Their safety rests on persistent depth
storage and refusal before any SDK receive when another thread owns dispatch.
They do not prove all native parsers reentrant, SDK callback/cost behaviour or
separate native output lifetimes. Engine replay chat, other audience modes, pings/drawings, complete
authority/play/seek/takeover, physical budget, receive cost, ten-seat continuation
and the accumulated replay/review/local landing gates remain open.

#### Copied loading chat inbox

`tagpu_recmp_chat_inbox.c` provides the loading/game-thread handoff before B2.
One fixed process-owned queue holds 4,096 copied observations; no engine
address is stored. Its ownership protects only copied match/host metadata,
queue values and the acceptance-time read of main GameTime. Held bodies make
no engine/SDK call, log, allocation or acquisition of another lock. The start
epoch survives loading reset, is claimed once at B2, and closes before live
level queues are freed. Bootstrap bounds all origin/recipient/target IDs by
the frozen roster. Overflow or a decreasing observed clock makes capture
incomplete, with the stock text image still restored for a valid wire packet.

The identified initializer now writes mask 7. The receiver requires loading/play
state 5/6, exact delivery and native sender/recipient validation, and can run on
either pump caller before the producer exists. The GUI source scope uses the
same copied current match. B2 transfers inbox entries before source advance.
Teardown transfers final entries and closes admission under one ownership
interval before final seals or queue free; later receivers cannot append after
that cut. Neither source nor recipient clocks are retimed to B2.

Normal/COVER builds pass. Production inbox/text-queue controls pass pending-reset
survival, once-only claim, zero receipt ticks, distinct source ticks, acceptance
clock/publication ordering, closure before final seals/queue free, 4,000
concurrent receipts, overflow without stock
suppression, changed-match and frozen-roster refusal. The exact production
receiver body accepts controlled state-5 game/foreign-thread calls without a
producer and restores the stock packet; wrong mode/state/row/span/controller/
network inputs refuse. These controlled boundaries do not prove native loading
display, native loading clocks or their saved-file application. Those gates
remain open, along with the full multiplayer landing.

MEASURED 2026-10-05 on the reference setup: the integrated inbox runs in a
three-human queued native *Town & Country* game, COVER, 20-unit limit, host
saving and both peers opted out. The final admission-closure build is retained
with its coverage map, SHA `fb6278986a471b2c160a390997f68b5d09dafb1ffd9d7acdd0e250defd68dcd4`.
Source IDs `689214863`/`689214857` send private serial 1 at ticks 285/383,
accepted by the other peer at 284/392, CRCs `507DBE27`/`51131C21`. Host
`689214861` sends its public serial 1 at 545, accepted at 511/507, CRC
`B7D817B9`. The host displays neither private line.
Its file closes at tick 764 with zero corrections. C/Python agree on seven
entries, eight latest-batch queries, two constructor roots and all 311
application envelopes / 71,127 bytes, CRC `0FE1FD27`, through reader seek.
The opted-out peers refuse contributions after host departure, repeating the
open continued-migration gap. All owned test instances/display are removed.
This verifies in-play integration and final inbox draining, not native loading
delivery or engine multiplayer playback.

#### Input acknowledgement used by the chat fixture

The engine's textfield editor is `0x4A7290`, with active edit index at GUIInfo
`+0x64`, caret at `+0x74`, and inline field text at `+0xB6`. The painter
`0x4A4D70` does not accept input. The character ring holds at most 29 queued
entries and drops a push when full. A field click initializes the editor and
resets that ring. The flip's trigger family snapshots UI and posts input;
posted messages have not been dispatched when the trigger family returns.
One fresh snapshot can therefore precede an already-requested field edit.

MEASURED 2026-10-05 on the reference setup, normal build, two native DirectPlay
peers on a private display with the shield on: after posting one character the
first field snapshot remains empty while the later caret read is 1; the next
snapshot contains that character. After posting another character the first
snapshot still has the previous text while the later caret is 2. A subsequent
fill reads exactly `probe`. Both edit index and GUI-object focus remain 2.
Both owned games/prefixes and the private display are removed. This establishes
an early-observation path; it does not alone establish the cause of every
previous driver failure.

The first COVER acknowledgement probe accepts `probe` exactly but fails while
replacing it: the existing backspace/delete loop leaves five characters. A
separate direct-control-character probe leaves `probe` unchanged on both peers:
`char:` followed by byte 8 is truncated by the token parser's <= `0x20`
delimiters, so that character never reaches the engine. This is not a test of
the engine's backspace editor. An existing-key control posts `end backspace`
on both native peers and observes exactly `prob`, caret 4, ring read/write 7/7
and focus/edit index 2. The End key queues editor code `0xF1`; backspace's
separate synthesized `WM_CHAR(8)` performs the edit. Both controls leave the
Mapping bit unchanged and clean up their games/prefixes/private display.

The second COVER replacement probe also refuses before submission: it never
acknowledges `p` while clearing `probe`. Its engine log places the field click,
End and four backspaces in the same input poll; a 0.2-second delay did not even
separate their posting. This does not alone prove where the edit was lost.
The GUI snapshot now reports `edit_index` and `caret` from inline
`GUIInfo+0x64/+0x74`, copied on the owning game thread. Neither value addresses
the gadget array. The existing `focus` string remains the panel's
`defaultfocus[16]`, not the actual edit index. A fill keeps a field already
selected as the editor; otherwise it observes the chosen edit index before
posting its clear/text batches. Its completion still requires exact field
text. No sleep supplies that ordering.

`tacli ui fill` now clears with End and at most four backspaces per batch,
waiting for each exact shortened prefix. It posts at most four text characters
per batch and waits for the same screen/gadget index, name and kind to
acknowledge the exact prefix before posting more.
Its timeout reports unacknowledged text, never successful completion. Callers
continue to compare `text` with `wanted` before submitting. This supplies an
observed-state ordering and a queue bound; increasing the old fixed sleep
would supply neither. The additive snapshot fields change no engine state layout, patch, transport,
simulation speed, exclusion or tolerance. The stopped native exploration gate
and wider multiplayer playback/seek/takeover gates remain open.

The third acknowledgement probe passes on the updated COVER build, two native
human peers, private display, shield on. Each peer fills `probe` exactly,
replaces it with the exact 43-character mixed-case line
`Mixed Case 0123456789 abcdefghij ABCDEFGHIJ`, then replaces that with
`+mapping`. Before Return, both snapshots report `TALK.GUI`, edit index 2 and
caret 8, matching the independent engine reads. After Return both return to
`ARMMAIN2.GUI` and LosType changes 15 → 14: the Mapping bit is toggled by the
engine command. The long line exceeds the ring's 29-entry capacity but exact
prefix acknowledgement bounds each batch. Both DLL builds pass; this native
run verifies the driver and the mode-change control. It records no complete
kind-11 file and does not close the stopped exploration-file/seek gate, native
multiplayer playback, audiences, physical bandwidth or ten-seat takeover.
Both owned prefixes and the private display are removed.
