# Demo recorder multiplayer slice

## Status

Started 2026-10-04. This is the **multiplayer slice** in the [recorder plan](demo-recorder.md#milestones-and-delivery-order), separate from S5's skirmish browser and controls. No multiplayer recorder change has landed. The current code starts recordings only in skirmish mode (`tagpu_recorder.c:tagpu_rec_capture`) and its puppet host accepts only skirmish controllers (`tagpu_replay.c:validate_header`).

The slice's product scope is a recording file sufficient on its own for playback, with contributions from every Impure peer: each player's effective perspective and authoritative state, communication, then playback and takeover. The [product contract](demo-recorder.md#multiplayer-contributions) requires contributions even from a peer that opts out of saving its own recording. A live multiplayer game has no rewind timeline.

## Two-peer baseline

MEASURED 2026-10-04 on the reference setup, retail content, two native-DirectPlay Wine peers on *Two Continents*, current local `main` build `446337c6`, private display. Both games carried `tagpu_recorder.on`. The host and joiner reached the same live battle with one commander per peer. `main+0x2A44` was `5` on both (network bit set). Neither wrote an `.imprec` file, consistent with the `MODE_SKIRMISH` start and continuation gates in `tagpu_rec_capture`. This is a baseline, not a test of an enabled multiplayer recorder.

After the host sent the network Pause, both peers reported paused. The host read GameTime 1339 and the joiner 1338. For each peer, its local player record (seat 0) at `player+0x8C` began with eight floats `(1000, 25, 0, 1000, 1, 0, 1000, 1000)`; its remote record (seat 1) began `(1000, 0, 0, 1000, 0, 0, 1000, 1000)`. Thus a recording on one peer cannot take the remote player's production and expense from that peer's player image. This repeats the earlier [remote-perspective measurement](demo-recorder-exploration.md#6c-remote-perspectives-require-authoritative-contributions), now against the S4d build. Seat 0 is local on each peer; a cross-peer comparison must match players by their transport ID, not by row number.

The first joiner prefix presented an empty provider list; the DirectPlay probe returned `0x80040154` at `CoCreateInstance(DirectPlay4A)`. Recreating that probe instance after the host launch populated all four providers and completed the two-peer game. No recorder conclusion rests on the failed prefix. The two probe instances and private display were removed after the measurement.

## Source boundary audit

`tagpu_recorder.c:olist_read` walks a unit's main and suborder nodes, and `id_of(RL_T_OWNED)` reads the vtable of a node's owned object. The skirmish lifetime argument at the head of that file applies to type-1 and type-2 owners. It explicitly names the type-3 air proxy that can delete its object independently. Three paths reach `olist_read`: normal `cap_orders`, the rotating `cap_verify`, and `dump_write`. The first two run only after a recording starts; `dump_write` can also run from an armed dump in a network game because `dumps_take` is called regardless of mode. Neither the node-vtable test nor the 4096-node cap proves the owned object remains live: both are tests on values read after following the pointer.

The by-design boundary for the network path is to derive a unit's owner from its `+0x96` pointer **as an ID bounded to the engine's ten player records**, then read that record's controller. Locally controlled owners (types 1 and 2) may use the present list lifetime argument; remote owners (types 3 and 4) need their orders from their controlling peer. This must cover all three callers, including dumps, before any network capture or dump can walk a remote list. Skipping a remote list makes the read safe but leaves the recording incomplete until its authoritative contribution replaces it.

## Contribution transport checkpoint

The work branch has a checkpoint, not a multiplayer recorder landing. `olist_read` now refuses a network owner's lists before following either head unless the owner pointer names one of the ten records and its controller is 1 or 2. That one boundary covers normal capture, verification and dumps. A puppet host has no network bit; its restored owned-object pointers are zero, so its list dumps retain their existing path.

`tagpu_recmp.c` carries bounded contributions through its own native DirectPlay application and session on one worker. The game thread copies the fresh engine-start match GUID, validated start host ID, native host address and every active human/hosted-AI primary ID, public key and ownership into `RecMpConfig`. Native ownership must agree with controllers 1/2 versus 3/4, and every local key must equal the process signer's public key. The copied configuration contains no engine pointer or primary-session interface. TA's `0x4C9840` receive wrapper and its source/destination writes remain stock; contribution packets never enter that pump.

The 64-byte version-2 envelope carries byte count, primary origin ID, source tick, sequence, kind, match GUID and stream GUID. The game-to-worker transmit queue and worker-to-game receive queue each have 256 slots of 4096 bytes with one producer and one consumer. Atomic publication orders copied packet bytes before slot reuse. The worker's sixteen native async completion contexts bound outstanding accepted sends. Full queues or a malformed/unauthorized logical stream end contributions incomplete. No transport call, worker join or queue lock runs on the game thread.

Each local human and hosted AI signs its primary origin and private identity against the public key already copied from that actual primary native player. A remote binding freezes its private SDK ID, session and stream. Data remains queued until all expected remote actors bind and every pending local identity announcement has been accepted before subsequent data sends. Native private membership changes and newly verified actors request reannouncement; a broadcast to an empty session cannot substitute for a binding. A worker retries only `DPERR_NOSESSIONS` during discovery. Retry scheduling establishes no completion fact.

Teardown releases the game reference and publishes stop without joining. A reference keeps the wake handle and copied channel storage alive through a concurrent worker finish. There is at most one channel worker per process, including one still finishing. The worker drains queued local work and native completions before closing its own session on the normal path; abort never claims delivery. When native membership has no private peers left, queued transmissions can retire only after every expected source has supplied its final prefix. A departed unsealed source cannot authorize that retirement. A 15-second stopped-worker drain deadline aborts incomplete; elapsed time proves no receipt, prefix or delivery fact. Normal teardown publishes a final prefix seal for each local actor before stop. A validated seal proves that received source prefix; it does not establish a complete recording. The consumption cut, final-seal receipts and retained-consumer API below now govern normal close. Game capture's file-context attachment and native authority application remain open.

Locally controlled human and hosted-AI players contribute their full normalized 520-byte player image and subsequent changed-word deltas with recording off. The recipient maps records by primary ID, independently of their private SDK sender. Source sequences are separate for up to ten origins. The game keeps authoritative caches only; it does **not** enable network recording, alter engine resources or materialize an `.imprec` file. `REC_FORMAT` is 5; the controlled container fixture below exercises that format.

The primary descriptor refuses `DPSESSION_NOMESSAGEID`, which removes native source identity. The private backend requires the native protocol, ordered guaranteed async sends and completion notifications on its own session. Its [Send contract](https://library.thedatadungeon.com/directx7/devdoc/live/directx/dpref_6dvd.content.htm) and bounded contexts establish requested transport support, not terminal recording completeness or a measured bandwidth budget.

### Authoritative player images and frozen seats

`tagpu_recmp_players.c` stores the existing recorder's entire normalized player image: the 0x14B-byte record, zero padding, and the process-lifetime PlayerInfo's 0xB9-byte tail. The existing `replay-layout.tsv` masks and unit-ID substitutions are unchanged. A separate owner-controller value distinguishes a human (1) from a hosted AI (2); the recorder's normalized controller byte remains excluded as before. This replaces the production adapter's earlier 32-byte resource-only samples without adding a recorder exclusion.

The copied `RecMpRoster` freezes primary origin, recorder-local seat and inclusive global unit-slot block for each active actor. Bootstrap requires unique origins, seats and blocks, ten fixed blocks of `each` slots plus sentinel slot 0, and each endpoint bounded by the engine's unit allocation. Block rank follows native IDs, not local player-row order (`0x485657..0x4856C0`, `0x4858A6..0x4858E0`). A contributed image must retain that origin's normalized first/last unit IDs. Remote queue authorization uses the same frozen range, independent of a recycled unit slot's current contents. The optional file context copies the 176-byte roster into its owned storage and requires it as `RT_MP_SEATS` in a mode-3 container header. Actual network capture does not yet create that context.

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

Normal close requires a validated source seal covering every frozen consumed sequence, all local final markers sent or legitimately retired, all outgoing receipts submitted or retired when no verified recipient remains, and zero queued/native-pending sends. Every source that remains active must also acknowledge each local final marker. An already-ended source needs no future local contributions for its perspective, but its prefix still has to cover the recording's own consumption cut. Thus the leaving recorder can finish while other players continue their match; it does not wait for their final match tick. The existing deadline only aborts incomplete.

A receipt is a 48-byte body naming the target primary origin, source stream, final sequence and exact 24-byte seal. Its control envelope has sequence/tick zero and does not advance that sender's DATA ledger. Its sending actor must already have a verified native binding. The target producer compares the whole receipt against the worker's copy of the accepted local final marker; a malformed or altered receipt refuses. Broadcast receipts for another producer have no effect. Duplicate exact receipts count once. Hosted human/AI actor receipt masks are bounded by the frozen ten-actor roster. Native completions include these 112-byte packets, separately counted in `receipts_sent`.

`retain` requires an already-owned channel reference. The game may transfer its sole receive consumer only after `end` publishes the cut and releases the game reference. The retained reference keeps copied packets, cut points and the wake handle alive after worker finish; `finished` publishes the worker's terminal result. The final release frees the channel. No engine pointer, primary interface or engine read enters this lifetime. The optional file context uses this transfer; game capture does not yet create it.

The actual production cut controls pass with event-selected interleavings. Two remote sources remain active; one sends a later DATA packet and seal after the consumer has ended. Both cuts resolve and all four final receipts validate. A retained consumer drains three late packets after worker finish, while its recorded sequence/ticks/ordinal remain unchanged. With one receipt withheld, cuts 2/2 and zero native pending still fail incomplete after the deadline. With one cut proof withheld, receipts 4 and zero pending still fail with cuts 1/2. Altering the final receipt checksum refuses immediately. These controls exercise the production channel and authentication/ledger code at controlled native-backend boundaries; they do not write a file or simulate a native COM completion.

The native production-channel fixture also passes after both primary interfaces close. Each peer receives sixteen DATA packets and four seals and validates four terminal receipts; each sends four receipts. The host completes 30 sends / 4240 application bytes and the joiner 26 / 2800, with pending/queue/unread/failure zero, final sources 2/2 and cuts 2/2. Identity reannouncements account for the remaining sends. Primary Close takes 2518/2519 ms in this standalone run, without a stock comparison. The test still retains its external barrier; the active-source cut control exercises closure without remote final markers. Physical bandwidth, file durability, all owner authority and replay remain open.

MEASURED 2026-10-04 on the reference setup, actual TA on *Town & Country*, two humans and one AI hosted by each, private display, both copied DLLs matching the tested build. Match `409EB149-7C91-457F-A7FA-D1A0A1EC3DEB` has human origins 619677058/619677056, AI origins 619677063/619677062 and private IDs 619698137/619698139. The host returns through ordinary surrender/main-menu; its worker finishes **before the joiner leaves**, with cuts 2/2 and four final receipts despite remote final sources 0/2. It reports 62 accepted source packets / 68 native completions, queued/pending/failure zero and one unread late packet. The joiner is still on `ARMMAIN2.GUI`; after its own menu exit, its worker reports 39 accepted / 45 completed, cuts 2/2, final sources 2/2, four receipts sent, queued/failure zero and two unread markers. Its received host final prefixes close source tick 205 (human sequence 23 / 1436 DATA bytes; AI sequence 39 / 2780 DATA bytes). Thus closure against a still-playing peer is verified; **no file is written**, and the future writer must retain and drain those proof tails without applying them beyond the frozen cut.

The first launch attempt stopped in `mp_lobby.sh` because its `tacli ls` inventory raced the standalone fixture's removal of an owned instance. No game started. Running the tests sequentially removes that fixture race; the subsequent live test above passes and removes both owned game instances/prefixes and its display. These results do not establish cross-address discovery, a full host-migration match, a network budget, all authority, file playback/seek or ten-seat takeover.

### Recording journal

`tagpu_recmp_record.c` provides a separate bounded journal for the world-file writer. One game-thread producer copies both locally published packets and remotely consumed packets, giving them one 64-bit ordinal and the actual producer/consumer application tick. The 16-byte metadata distinguishes production from consumption; the original 64-byte source envelope and payload retain their source clock, sequence and stream unchanged. Advancing a source closes its prior tick, but its locally produced seal receives the current capture tick as journal metadata. A post-end proof-tail take never appends another application record.

Its copied 112-byte roster contains the fresh match, recorder origin and up to ten primary origins/local roles; it contains no key or engine address. The journal uses a 16 MiB SPSC byte ring. Atomic publication follows every copied byte, including a wrap marker; only the writer's read publication gives that room back. A full or malformed journal fails locally without overwriting unread bytes. The channel detaches that failed journal and continues its mandatory contributions to other recorders. An explicit game-thread detach also precedes ending/freeing a journal while the contribution channel remains active. A journal can be claimed for one channel only, before any packet publication/consumption.

The producer's end publication is its last access; a writer frees only after acquiring end and draining the journal, or discarding a failed journal. No worker accesses the channel's game-owned journal pointer. `RecRing` has an optional copied contribution context and `tagpu_recwriter.c` drains it as described below. Game capture does not yet create that context or enable network files; game-produced file mapping, authoritative-field materialization into the engine and all player views/communication remain open. The reader's separate authority queries rebuild semantic player/order state after seek.

The journal controls pass copied-body and odd-length physical-wrap checks, a bounded overflow, producer-end hand-over and role/match/span/kind refusal. The cut controls attach the actual journal to the production channel: they retain two consumed packets and three local production records at recorder tick 100, then freeze it. Later proof-tail packets drain without adding application records. A full local journal also leaves the contribution channel healthy, so other recorders do not lose that source when the local recording fails.

MEASURED 2026-10-04 on the reference setup, the standalone native production-channel fixture writes each peer's journal after both primary interfaces close: 40 records / 4016 bytes per file, with twenty local productions and twenty remote consumptions covering the four human/hosted-AI origins. The C fixture rechecks each source ledger; an independent Python decoder reads the persisted bytes and checks consecutive application ordinals, nondecreasing application ticks, roles, source sequences, cumulative DATA CRCs and final seals. An altered DATA byte and a truncated tail both fail validation. These fixture journals have no world snapshot or `.imprec` header and are not playable recordings.

A separate producer/writer-thread control copies and validates 15,000 alternating maximum-size and odd-length records across physical wraps. Consumed-batch events bound outstanding bytes; after acquiring producer end and draining, the writer frees the journal and signals an independently owned completion event. This exercises the actual final-access/free ordering without relying on how quickly either thread runs.

### Contribution file container

The work branch writes/reads format 5. `RT_MULTIPLAYER` carries the copied 112-byte roster in a mode-3 header; both writer and readers refuse a network mode without that roster or a roster in another mode. `RB_CONTRIBUTIONS` carries consecutive 16-byte application metadata plus exact source packets, with no ring padding. Its block timestamps are zero: packet metadata retains application/source clocks, and these blocks are not world seek snapshots. After game hand-over, proof-tail records use phase 3, zero application ordinal and zero application tick. They enter the persisted source ledgers without entering the world copy or extending application cuts.

`tagpu_recmp_file.c` owns the attached journal and a retained channel reference. It is created before channel publication/consumption and transferred into `RecRing` before writer start. The game ends that context before its final ring publication. Ending a local save detaches the journal and publishes a distinct local-stop state; the file writer cannot take the channel consumer or stop game contributions in that state. Ending the channel first permits the sole receive consumer to transfer after journal drain. Context end is the producer's final access; the writer acquires it before freeing either the journal or the retained reference. No engine allocation or interface is kept alive by this context.

The writer independently reconstructs bounded per-origin source ledgers from copied records, including stream/match/role checks, consecutive application ordinals and nondecreasing application ticks. An `RT_MP_END` footer contains version/count, the application-record count and ten 40-byte consumption-cut rows. Local and unused rows are zero. Remote rows must equal the cuts reconstructed from consumption records, using the separate remote-consumption ordinal; a local source must have its final seal and every remote source must have a seal covering its application cut. The writer also requires the channel's terminal result. Missing transport proof or failed local storage abandons the file without a complete end block. C open/summary/info and Python segment parsing validate persisted contribution blocks and that footer before reporting a complete network container.

MEASURED 2026-10-04 on the reference setup, `tools/replay/recmp_file_test.py` runs the actual file writer, compressor, path lock and C readers with the production channel at controlled native boundaries. The normal synthetic world file is 2,985 bytes: 21 unit slots, `each=2`, four described seats and their copied origin/block mapping, world ticks 100/101, sixteen application records (seven remote consumptions) and four late proof records. Both readers validate it; the C reader reads both world ticks and seeks back to 100 with the world player image unchanged by proof-tail DATA. Missing cut proof, a missing final receipt, a full journal and a local save stopped while contributions continue each produce an incomplete file whose durable tick-100 world prefix remains readable. Altered DATA, altered application ordinal, a truncated logical tail, an omitted footer and a changed application cut are refused by C open/summary/info and Python, even after rebuilding the outer block checksums.

This test launches no TA game and uses no engine capture. Its world and player/order images are synthetic. It verifies the contribution container, ownership path and the semantic player/order history below, not a faithful game-produced multiplayer recording or network playback. `tagpu_rec_capture` still starts only skirmish recordings and the puppet host still refuses network headers.

The mode-3 header now requires a separate packed `RT_MP_SEATS` (tag 28): the validated 176-byte frozen roster, in the same origin order as `RT_MULTIPLAYER`. The file context validates and copies it before journal attachment; modifying the caller's roster afterwards changes nothing. Writer and both readers require its slot/each counts to match the world allocation, distinct recorded seats described by `RT_PLAYER`, and local-versus-remote controller roles to agree with the source roster. The writer additionally requires exact agreement with its owned context. Two multiplayer segment headers with different source/seat mappings cannot share a chain shape. This metadata provides identities and bounds; it does not materialize source state or prove initial authority is available.

The controlled file fixture verifies all five normal/failure lifecycles with that copied mapping. Twenty-three rebuilt-CRC header mutations cover missing/duplicate/short mappings, version/count/slot/each mismatches, unknown/aliased origins/seats/blocks, unaligned/outside/wrong-sized intervals, nonzero unused rows, missing/duplicate/short seat descriptions, wrong ownership and a falsified network mode. C open/summary/info and Python all refuse. The falsified-mode control exposed the file-info path's mode-dependent validation; file info now validates the entire header before choosing its scan. This is bounded file parsing, with no engine read, write or patch.

### Source-local player rows

Player contribution bytes retain the owner's local player-row coordinates. The recorder's frozen roster alone cannot interpret those coordinates when another peer orders its rows differently. `tagpu_recmp_seat_map` accepts two copied, validated rosters with identical origin sets and fixed global unit blocks, independent of actor-array order. It maps active rows by native origin, pairs vacant rows in ascending order and fixes dummy row 10. This is an eleven-row bijection, preserving even inactive vector entries instead of dropping them.

`tagpu_recmp_player_map` maps the five 11-byte vectors at player `+0x108`, `+0x113`, `+0x11E`, `+0x129` and `+0x134`, and self row `+0x146`, in a copied 520-byte normalized image. It first requires the self row and normalized unit-block endpoints to agree with that source origin. Every other byte is retained, including PlayerInfo, start position and scoreboard rank. The destination is published only after validation and can alias the source image. This utility reads no engine memory and writes no engine state.

The player fixture executes 640 roster permutations over all actor counts 1–10, reversing the destination actor-array order. All 3,520 player images return byte-exactly through the inverse map; active identity, vacant/dummy rows, unchanged other fields, in-place mapping and destination canaries pass. Fifteen refusal controls leave the output unchanged. Exact-size copies ending at a guard page and invalid lengths/capacities with an inaccessible source exercise the span bounds. The immutable source-roster cache also refuses wrong spans, versions, kinds, sequence, origin and stream, retains copied values after caller mutation, and refuses duplicate announcements without replacing the map. The existing cache/journal checks still pass in C and independent Python. Python's file history now independently implements roster validation and the same eleven-row permutation.

Source kind 7 carries the owner's copied 176-byte frozen roster at sequence 1, before its first full player image at sequence 2 and before orders. Each origin announces the whole source peer's roster once. Exact origin sets and fixed global block endpoints must agree with the recorder; actor order and local row assignments may differ. The game adapter validates each local image against its frozen source map and each received image against that sender's map. Raw delta baselines stay in source coordinates. Maps belong to the game thread and are freed at failure/level end; the worker still owns only copied packets.

File history owns separate source and application map caches. A proof-only roster validates a proof-only full image but publishes neither mapping nor player authority; both C and Python fixtures check this. Produced/consumed player queries now return recorder-coordinate vectors/self row, retaining source stream/tick/sequence and application tick. The wire version is 2, shared by start tokens, primary keys, private identity and envelopes. Experimental wire-version-1 contributions are refused; the 176-byte roster schema and file metadata schema remain version 1, and `REC_FORMAT` stays 5. No recorder exclusion or engine patch changes. Unit owner/killer indices, projectile owner indices, explored bits and declared Impure player IDs still require their own remapping audit; game network recording and engine replay application remain disabled.

### Semantic application authority

`tagpu_recmp_history.c` validates source bodies as well as their envelope ledger. Its frozen roster bounds each order slot by its origin's inclusive unit block. Full player baselines, exact masks and block IDs use the existing player codec; order chunks use the existing complete-transaction cache. A seal cannot close an incomplete source order transaction. Invalid input poisons the history, and no state can be queried afterwards. Writer and C open/summary/info use this validation; Python's independent `History` decoder enforces the same body and transaction bounds. Legacy resource-kind records remain opaque ledger entries and establish no complete player or order authority.

Every history owns two bounded player/order cache pairs. Source caches consume all records, including proof tails. Application caches consume only produced/consumed records, preserving the last complete image/list and its source stream/tick/sequence and actual application tick. The proof validator uses the last application tick internally while it verifies later source packets; it never assigns those proof packets a recorded application time or publishes them. Each order cache has 131072 nodes, two list headers per bounded slot and ten 4096-node staging buffers. No engine pointer enters this storage.

`recr_mp_player` and `recr_mp_orders` reconstruct authority at the reader's current world tick. Forward queries consume new application records only; a seek, changed segment or refreshed file index rebuilds from that segment's application prefix. Contribution blocks are read with bounded sizes and checked checksums, using a separate owned payload so querying cannot invalidate a partly read world block. The file cursor is restored after querying. Unknown baselines/lists stay unknown. These APIs return copied values separately from `RecWorld`; they do not merge authority into the world or write the engine. Python `History.at(T)` independently rebuilds the same application prefix.

The controlled 2,985-byte file, with 16 application records (seven remote consumptions) and four proof records, reconstructs four synthetic 520-byte player images at application tick 100, with source ticks 100 for the local human/AI and 8 for the remote human/AI, sequence 2 and the correct source streams. The remote peer describes rows 7/4/0/2 for origins 11/22/55/66 while the recorder uses rows 0/1/2/3; all five vectors, including vacant/dummy entries, and each self row are checked through playback/seek. It retrieves all 88 bytes of a recorded nonempty local secondary list, including after seek. An empty remote list at source tick 8/sequence 3/application tick 100 remains the last complete list when a 46-node replacement begins at application tick 101. Only the proof tail supplies its final node and a changed player image; source validation reconstructs 46 nodes and the changed image while playback/seek still return the previous complete list and player image. Tick 99 and an unreported slot/list remain unknown. All five file lifecycles and the 23 header controls pass. Twenty-eight additional body mutations rebuild both outer checksums and source-prefix seals; the raw framing/cut ledger accepts each, but C open/summary/info and Python reject the malformed source rosters, missing/repeated announcements, player self rows/masks/baselines/block IDs, order ownership/bounds/continuations and a seal over an incomplete transaction. No tolerance or exclusion changes.

The earlier format-5 container checkpoint passed the actual TA `secondary-orders` regression through live recording, puppet playback, in-place restore and fresh-process seek: complete tick-1080 recording, image/event comparisons and three exact round trips at 481/511/541. The mapping checkpoint's later run ends complete at tick 1080 with 1080 iterations; world/radar/event/count comparisons and round trips at 361/421/451 pass, poisoning 26 tables with zero residue and equal before/after dumps. Its images inventory gate fails at two bytes of `main+0x531`, the active GUI heap pointer. Disassembly and full pointer measurements are in the [engine map](../exe-reverse-engineering.md#the-active-gui-pointer-and-the-images-inventory-disassembled-measured-2026-10-04). A proposed four-byte `leave` row awaits the owner's exclusion decision; the later regression is not clear and no exclusion or tolerance is applied. The semantic reader also opens, summarizes and inspects that real skirmish file. The harness's 39 logic tests pass after refreshing the coverage map for the new reader functions. These are skirmish reader/regression results, not native multiplayer replay coverage. Engine application, complete unit/effect/Impure authority and game-produced network record/play/seek remain open.

### Source-roster native verification

The first wire-version-2 four-actor live run produced both local sequence-1 rosters and both sequence-2 full players on each peer, but delivered no contributions: the join worker returned SDK `DPERR_TIMEOUT` (`0x887700F0`) before opening its private session. Its contribution producer stopped at tick 15. The host had opened a private session but received no bindings and exhausted its bounded 256-packet transmit queue at tick 816. This failure does not establish live source-map exchange. The adapter now names the exact failed native open stage in its error log, without adding retries, changing deadlines or imitating success. The diagnostic rerun below passes. It does not establish the timeout's cause or a by-design fix; private-session startup reliability remains open. No deadline, retry or timing mitigation was added.

MEASURED 2026-10-04 on the reference setup, a new native four-actor *Town & Country* game, both copied DLLs identical to the tested build, private display. Host human 632031350 and AI 632031347 advertise the same source roster checksum `606D73BA`; join human 632031348 and AI 632031346 advertise `3F39675A`. Each published roster equals its recipient's exact checksum at sequence 1, and all four source-tick-1 full images match raw source checksums/provenance at sequence 2. On each recipient the remote human maps source self row 0 to local row 1; the hosted AI maps 2 to 2 or 3 to 3. Host receives its remote maps at application tick 54 and join at 17: authority before arrival remains unknown. Both source seals validate. After ordinary exit the host reports 80 accepted / 86 completed, cuts 2/2 and four receipts; the join reports 54 / 60, cuts 2/2 and four receipts sent. Both end with queue/failure zero. The joiner was still on `ARMMAIN2.GUI` after host departure, but this run ends it afterward and does not verify continued host-migrated play. No game `.imprec` is created and no game playback/seek is run.

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
async sends, with sixteen fixed contexts. An accepted `DPERR_PENDING` transfers
the copied packet to the SDK; only a validated SENDCOMPLETE releases that
context, and only success counts delivery. A seventeenth pending send and an early Close
return BUSY. An abort ends only the worker's private interface and makes no
delivery claim. The current backend supports TCP/IP only. Worker discovery retries are installed; cross-address discovery, host migration and production terminal manifests remain unverified.

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

### Gaps before the first code landing

- Verify cross-address discovery, player leave and host migration. The separate application/session removes the shared receive-signature and shared send-queue obligations by construction, but does not prove physical network bandwidth cannot delay game traffic. Measure and enforce that budget.
- Integrate the frozen consumption cut, validated final receipts and retained consumer into the file writer. Preserve semantic application history and partial-transaction boundaries through teardown and seek. Packet-prefix/transport closure is not durable-file completion.
- Carry the remaining player authority and materialize contributed resources/orders; retain source tick, sequence and recorder application order in the file and its seek snapshots.
- Decide unit corrections from a bandwidth measurement, as the product plan requires; no measured budget is inferred from the two-commander run.
- Run the network recorder/playback fixture and the skirmish replay gate on the finished accumulated diff. The transport test and network dump do not replace those gates.

## Work order

1. **Network capture boundary.** Prove the lifetime and bounds of objects read from type-3 unit owners, especially the air order proxy whose owner can delete it independently (`tagpu_recorder.c`, *What it reads*; [order research](demo-recorder-s2-orders.md#2-orders-on-a-type-3-puppet)). Map the capture tick against the network pump and owner sends. A probe must distinguish a remote copy from an authoritative local record. The header currently writes `RT_MODE` as `MODE_SKIRMISH` (`tagpu_recorder.c:header_build`), so a network file also needs a deliberate format rule. A file with plausible but wrong remote state is not a completed landing.
2. **Contribution envelope and authority.** Give each contribution an explicit version, bounded length, origin transport ID, source tick and sequence, and recorder application order. Map source ticks to the recording's ticks; the paused peers above differed by one. Capture owner-authored resources, orders and other missing state, and verify delivery with two peers before relying on any field. Continuous presentation samples can be superseded; discrete orders, selections and chat require ordered delivery or repair ([protocol constraints](demo-recorder-exploration.md#perspective-data-and-network-budget)).
3. **Perspectives and communication.** Record explored-map history, camera, cursor, clicks, selections, public/team/private chat and pings with their audiences. Sight is recomputed from recorded units, as the product contract specifies; it is not contributed. S5's UI work consumes some of these streams; the recording format has one owner at a time, coordinated before `tagpu_recfmt.h` changes.
4. **Playback and takeover.** Host up to ten recorded seats without assuming a spare viewer seat ([puppet-host limit](demo-recorder-s2-host.md#13-the-limit-a-ten-seat-recording-new)), restore each seat's controller and authoritative state, then take over one player with the others as AI. Run record, puppet, seek and takeover checks on a real two-peer game and extend them to the ten-seat case. Preserve the existing replay round-trip gate for any simulation change.

Step 1 is the research gate. Network recording remains disabled until the first code landing combines its safe capture boundary with enough of step 2 to carry authoritative remote state. That landing needs a two-peer fixture, a real network recording and a checked account of every remote field it carries. Presentation and playback can follow as separate landings, but a partial file must not be presented as a faithful multiplayer replay.
