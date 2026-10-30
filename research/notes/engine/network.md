# Multiplayer and the network

The network receivers, unit identity on the wire, the unit sync, match identity and the player metadata.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### Multiplayer contribution wrapper [DISASSEMBLED and MEASURED 2026-10-04]

Work branch checkpoint; native observed-world files are written whenever the game is recorded, which is by default ([GPU status](../gpu-status.md) §2.6e), with owner authority in a separate contribution ledger and engine playback unfinished. See the [multiplayer slice](../tadr-port/demo-recorder-multiplayer-slice.md#contribution-transport-checkpoint) for its runtime evidence and open protocol gates.

- `0x4C9840..0x4C9885`: the complete 69-byte receive wrapper was read during the earlier shared-session feasibility work. It is no longer redirected by the contribution channel. The trace helper `0x4C9740` is a single `ret`. The wrapper reads the interface at network-context `+0x4C5`, receives through vtable `+0x64`, and uses context `+0x4B5/+0x4B9` for source/destination outputs. The raw and reliable callers reach this stock boundary; contribution packets use a distinct native application/session and never enter its pump.
- The context is `main+0x14`, so those fields are main `+0x4D9`, `+0x4C9` and `+0x4CD`. `HAPINET_uninitmultiplay 0x4C9B70` calls interface `Close` at `0x4C9B92`, then `Release` at `0x4C9B9E`, then zeroes its field at `0x4C9BA1`. An AddRef cannot preserve that session. The isolated contribution worker retains no primary interface or context pointer. The teardown's four direct callers are `0x4446F4`, `0x450E06`, `0x4523C8` and `0x46C1B2`. The `0x450DD0` path calls `0x46BF20` and bypasses uninitialization on a nonzero result (`0x450DF3..0x450DFA`), then clears network bit 0 at `main+0x2A44` (`0x450E10`). `0x450D80` is a separate preceding function, ending at `0x450DC0`. A native session GUID is not assumed to identify one level; the fresh start GUID supplies match identity.
- The same teardown also closes the original DirectPlay interface at context `+0x4C1` (main `+0x4D5`): vtable `+0x10` at `0x4C9BB4`, Release at `0x4C9BC0`, and zero at `0x4C9BC3`. The following interface fields, context `+0x4CD/+0x4C9`, are released at `0x4C9BD6/0x4C9BEC` and zeroed. Context `+0x4D1` is freed through `MEM_Free 0x4D85A0` at `0x4C9C00`; `+0x4D1/+0x4D5` are then zeroed. A hook deferring only `0x4C9B92` would leave a second Close path intact. This audit has not established a safe deferred engine shutdown.
- `0x4CA780..0x4CA7B7` is the stdcall network-player destroy wrapper (`ret 8`): context and native player ID are its arguments, the interface is context `+0x4C5`, and vtable `+0x24` is called at `0x4CA7A3`. It returns 1 only for HRESULT zero, or 0 for a null interface. A whole-`.text` direct-call sweep found one caller, `0x452E59`. Its enclosing player removal is `0x452CC0(dpid)`: the prologue searches ten native IDs through `0x44FFD0` (`0x452CD9`), resolves a matching row through `0x44FE40` (`0x452CFF`), and refuses a null/inactive row, a controller outside 1/2/3, or row `+0x146` equal to 10. It saves PlayerInfo `+0x97` bit 0 at `0x452D63..0x452D6E`. The span at `0x452D72..0x452DBA` iterates all ten `0x14B` player rows, clearing row `+0x113/+0x108` entries for active local controllers 1 or 2; the following native-ID scan is bounded to ten rows (`0x452DC1..0x452E0B`). It passes that scan's row to the kill-all root `0x486F10` at `0x452E17`. At `0x452E22..0x452E81`, main `+0x2A44` bit 2 set makes an active local controller 1 or 2 skip both DestroyPlayer and the row reset. With that bit clear it calls DestroyPlayer for those controllers, then `0x463C60`, zeroes row `+0/+0x0C`, sets `+4` to `0xFFFFFFFF`, and decrements main's word `+0x2A3C`. The following cleanup clears PlayerInfo `+0x9D` bit 2 through row `+0x27`, clears row `+0x108..+0x112`, and tests the object at main `+0x391E9` through `0x435100` before a conditional `0x46C620(3)`. These are disassembled branch facts; all indirect routes to native DestroyPlayer have not been established; the loader/game-thread removal callers and later host migration remain covered by the existing session/host audit below. No engine teardown patch follows from this partial audit.
- At a network game's end a joiner clears the other remote players' rows before their recorders stop. MEASURED 2026-10-06 on the reference setup under Wine, five Impure peers each seating one AI, only the host recording: after the host's final seals had reached every joiner, each joiner's rows of every other remote player read active `0`, ID `0xFFFFFFFF` and controller `0`, at GameTime 666..669, while those players' recorders were still sending. The row writes match the removal `0x452CC0`'s reset above; that it ran once per row is [INFERRED], not traced. The contribution recorder reads these rows as data: a player image whose seat no longer holds its origin with controller 3 is refused, and that refusal ends only the refusing peer's own recording (`tagpu_recorder.c:mp_relinquish`, [the slice note](../tadr-port/demo-recorder-multiplayer-slice.md#channel-failure-handling)).
- `0x46BF20..0x46BF2D` returns whether global `0x51E58C` is nonzero; its two direct callers are the cleanup paths at `0x450DF3` and `0x4523B5`. `0x51E58C` is a `reporter.dll` module handle: `0x46C002` pushes its string at `0x507AF8`, `0x46C007` calls `LoadLibraryA` through import `0x4FC0D4`, and `0x46C00F` stores the return. `0x46BF30` resolves `_RIEnable@4`, `_RIGetVersion@4`, `_RIInitializeEx@8`, `_RIReport@40`, `_RIReportGameChat@8`, `_RISetCallbacks@8`, `_RIIntervalTimer@0` and `_RITerminate@0` from the strings at `0x507AEC`, `0x507ADC`, `0x507AC8`, `0x507AB8`, `0x507AA4`, `0x507A90`, `0x507A7C` and `0x507A6C`, respectively, through `GetProcAddress` at import `0x4FC094`. Failure/initialization paths free the module and zero `0x51E58C` at `0x46C119/0x46C11F` or `0x46C143/0x46C149`. Cleanup `0x46C190` first calls `0x4C9B70` if the handle is nonzero (`0x46C1A1..0x46C1B2`), then calls the terminate export, `FreeLibrary` through `0x4FC0C0`, and zeroes the handle at `0x46C1CF`. A whole-`.text` operand sweep found the one load-result writer and those three zero writers; the handle is not a match ordinal. The preceding `0x46BF47..0x46BF63` copies 16 bytes from the object at main `+0x4E5`, field `+0x10`, into main `+0x39201`; `0x46BF75..0x46BFB5` compares it with constants `0x4FCD98`, `0x4FCDB8` and `0x4FCDC8`. This pass did not establish that object's type or the copied field's meaning.
- The contribution resource capture reads the fixed ten player records at `main+0x1B63`, stride `0x14B`, their controller at `+0x73`, transport ID at `+4` and the 32 resource bytes at `+0x8C`. The local row is bounded from main `+0x2A42`; a received human's or hosted AI's resource sample is matched by transport ID to a type-3 row. The two peers' row 0 is local on each, not the same player. Measured tick-1 owner production was energy 25 / metal 1; remote stock production remained zero, and a real solar changed the host's energy production to 45.
- The network list reader first maps a unit's owner `+0x96` to one of those ten records by arithmetic. With main `+0x2A44` bit 0 set, it refuses to follow unit `+0x5C/+0x60` unless that bounded owner is type 1 or 2. For a type-3 owner it can instead copy a complete authoritative cache list, requiring its origin to match player `+4` and its exact birth to match the declared `s_hitStamp` row. The stamp column has the permanent allocation and load-before-capture ordering documented beside `hit_layout` in `tagpu_patches.c`; unknown/lower-bound stamps cannot authorize a queue for a recycled slot. This covers `cap_orders`, `cap_verify` and `dump_write`. It supplies the missing lifetime boundary for node `+0x52`; the type-3 air proxy can delete that object independently of the node (the order-object map below). A measured network dump retained the local slot 1501's one-node list and omitted both remote slot 1 lists.
- The order-contribution receiver reads player `+0x67/+0x6B`, the inclusive first/last unit pointers assigned at `0x4858BD/0x4858E0` (the unit-block ordering section below). Both become IDs in `[main+0x14357, main+0x14357 + main+0x14351 * 0x118)` before the source's slot is authorized. No live unit pointer is needed for that authorization: a private queue may precede its stock create. The list reader separately requires an exact matching incarnation before using that queue.

Recheck with `objdump -d -M intel --start-address=0x4C9720 --stop-address=0x4C9885 pristine/TotalA.exe.pristine`, `0x4C9B70..0x4C9C20`, `0x4CA780..0x4CA7B7`, `0x452CC0..0x452ECA`, `0x450DD0..0x450E1A`, `0x452370..0x4523DF`, and `0x46BF20..0x46C1D5`; read the export strings with `objdump -s --start-address=0x507A58 --stop-address=0x507B08 pristine/TotalA.exe.pristine` and imports with `objdump -p`. The caller/writer sweep is over `.text`; runtime resource and dump conditions are in the slice note. A distinct native application/session separates contribution packets from every engine frame without filtering the primary receive. The native file writer now validates terminal proofs and persists observed-world/contribution namespaces; engine authority materialization remains open. The engine-start match ID is described below. Native sender identity authorizes each locally hosted AI separately, and source sequences detect gaps and collapse duplicate physical deliveries; neither condition alone establishes a complete recording.

The signature audit also read these transport paths; it does not yet establish all raw-mode senders:

| Address | Established fact |
|---|---|
| `0x4C9840` callers | Two direct calls in retail `.text`: `0x44FA63` from `0x44F9C0`, and `0x4535B1` from the raw receive branch. `0x462F30` reaches it through `0x44F9C0`, at `0x4630DE` and its grown-buffer retry `0x46314D`. |
| `0x44F9C0` | Receive/decode wrapper, `ret 12`. Pending decoded bytes are copied from its own buffer before another Receive (`0x44F9D3..0x44FA4F`). Its DirectPlay call is `0x44FA63`. A non-system packet whose first byte is neither 3 nor 4 returns success **without decoding it** (`0x44FA91..0x44FAA8`); it is not rejected. A 3/4 packet is length-tested, its encrypted bytes summed and XORed with their byte offsets (`0x44FAC4..0x44FAF7`), then its checksum at `+1` is compared. A 4 calls `0x4D1480` between `0x4D1810` and `0x4D1800`; this audit did not read those bodies or establish their lifetime mechanism. A 3 copies the body from `+3` into the decoder's own buffer (`0x44FB30..0x44FB4C`). |
| `0x44FC44..0x44FD14` | The encoded send branch selects header byte 4 at `0x44FC5E` when compression saves bytes and main `+0x4ED` is zero; otherwise it copies the original body and selects 3 at `0x44FC75`. It XORs/checksums bytes, writes that selected header at `0x44FCB0`, and sends through `0x4C97B0` at `0x44FD0D`. The surrounding compression call is `0x4D0F60` between `0x4D1810` and `0x4D1800`, whose bodies were not audited. The optional send-drop branch reads main `+0x37F35` and calls CRT rand `0x4E4870` with arithmetic helpers `0x4E4400` and `0x4E4440`; it may skip the send. These are reads, not recorder patches. |
| `0x4C97B0`, `0x4C9800` callers | The general wrapper has three direct retail calls: `0x44FD0D`, `0x451DAC` and `0x451EFD`. The adjacent guarantee-forcing wrapper `0x4C9800` has no direct call in that sweep. No claim about indirect calls follows from it. |
| `0x463173..0x4631CB` | After the receive, the reliable layer copies context source/destination into its own `+0xC/+0x10`. Non-system data is length-tested, then its first dword is tested against `0xFFFFFFFF` before source lookup `0x462D90`. This audit did not prove its sequence semantics. |
| `0x453D40` / `0x45473F..0x4547AD` | The pump maps transport IDs to player rows and separates system source 0. The game-message branch reads main `+0x391F1` and admits byte codes through mask table `0x512BC0`: bit 1 in state 5, bit 2 in state 6, bit 0 otherwise. The established receive-mask initialization `0x451FD0` remains the fuller map above; a raw signature cannot be justified solely by the encoded send's 3/4 prefix. |

Recheck `0x44F9C0..0x44FBFA`, `0x44FC44..0x44FD14`, `0x4630B9..0x4631CB`, and `0x453D85..0x4547AD` in the pristine executable. These negative results prevented a whole-engine signature proof for the earlier shared-channel filter. The isolated channel removes that filter and leaves these paths stock.

### Multiplayer start action and match identity [DISASSEMBLED 2026-10-04]

The contribution checkpoint carries a fresh match GUID in the engine's existing
start action. A DirectPlay session GUID is still checked in the bounded descriptor,
but does not identify a level. No capability or version negotiation is added.

| Address | Established fact and use |
|---|---|
| `0x490B30`, `0x490B89..0x490B99` | The state setter writes main `+0x391F1`; its state-3 table case assigns handler `0x496CE0` to main `+0x391F5`. The state-2 case at `0x490B77` assigns `0x496BB0`. |
| `0x496BB0`, `0x496C95`, `0x496CA4` | The lobby handler [INFERRED name] checks main `+0x2A44` bit 2 and lobby fields `+0x2BBE`, `+0x2BBF`, `+0x2B4C`, then writes state 3 and handler `0x496CE0`. Its earlier mode-1/mode-2 branches select the campaign/skirmish load handlers. |
| `0x496CE0..0x496DA4` | The start handler [INFERRED name] selects the controlled row through main `+0x2A42`, takes PlayerInfo from player `+0x27`, and tests Info `+0x97` bit 0 at `0x496D0C`. Only that host branch builds byte `0x08` on its stack (`0x496D15`) and sends `(player+4, &byte, 1)` through call `0x496D25 → 0x451DF0`. A whole-`.text` sweep found no direct E8 call to the handler; its state-pointer assignments are the entries above. A 22-line disassembly lookback at every direct call to `0x451DF0`/`0x451BC0` found this one immediate byte-8 constructor; that is not a proof about computed constructors. Both branches select state 5 and handler `0x497F40`, at `0x496D65/0x496D75` or `0x496D87/0x496D9D`. |
| `0x451DF0`, `0x451EE0`, `0x451EFD` | Generic sender, stdcall `ret 12`, validates an active local type-1/type-2 source and tests the network bit. Reliable mode calls `0x461990`; raw mode calls `0x4C97B0`. B8 owns its entry; the recorder redirects only the start call, preserving B8's path. The targeted sender `0x451BC0` is stdcall `ret 16`, validates a local source and remote destination, and uses its raw call at `0x451DAC`. |
| `0x461990`, `0x462710`, `0x4628D0` | Reliable queue path: the wrapper forwards to `0x462710`, whose copy routine `0x4628D0` proves old payload plus incoming bytes fits `0x42A` (1066), copies the exact span into queue-owned memory at record `+0x14+offset` (the bound at `0x462911`, the copy at `0x462919..0x46292C`), and stores its length. The caller's 25-byte stack start, and the 97-byte identified text built on the sender's stack, is no longer needed after return. The queue has a `0x400` entry bound. |
| `0x451FD0`, `0x452040`, `0x4520BE..0x4520D4` | Packet-table initializer. ESI is 1 at `0x452040`; `0x4520BE` writes that fixed length to `0x512AF8`, the `0x08` entry in length table `0x512AD8`. Mask `0x512BE0` is 7 (all three pump phases), set at `0x4520CE`. The recorder redirects the six-byte length store to write 25 and resumes at `0x4520C4`, so reinitialization retains the extended format. |
| `0x453D40`, `0x453E14`, `0x453E84..0x453EA2` | The retail receive pump keeps the message at stack `+0x10`; B3's fallback receive note repairs this pointer after realloc and retains the delivered length in TLS. The recorder's owned receive path assigns that slot from its private allocation instead: it redirects the pump's entry `0x453D40` (`A1 E8 1D 51 00`, `mov eax,[0x511DE8]`) to a wrapper that runs those five bytes and resumes at `0x453D45` (`sub esp,0x51C`), and the receive call at `0x453D94`. The queued receiver's copy `0x46354F..0x463567` does not check its caller's capacity; every length the splitter hands it is a `u16`, the `0x2C` size read through `0x415DC0(0x10)` (`0x463919..0x463923`, the call at `0x46391F`) or the table word at `0x463931` (`mov ax,[eax*4+0x512AD8]`), so the private allocation is 65,536 bytes. Stack `+0x14` is the source row **index**, initially 10 for no match, not a player pointer. The pump derives source player EDI from it at `0x453E9B`. Recorder metadata bounds that saved EDI to the ten-row array before reading native ID `+4` and process-lifetime PlayerInfo `+0x27/+0x97`. |
| `0x45473F..0x4547AD`, `0x4547AD..0x45483F` | Pump phase mask and player admission. States 5/6 test mask bits 1/2; other states test bit 0. The source row is EDI and destination row is EBX. The recorder accepts start metadata only on the GUI/game thread, in network mode 3 and states 2/3; a late loading/play delivery does not replace its token. |
| `0x455F9C`, `0x4553BE..0x4553D5` | The dispatch-table slot for `0x08` points to `0x4553BE`. Its first instruction loads main from `0x511DE8` (five bytes; the observer runs them and resumes at `0x4553C3`); the body clears stack `+0xF0`, sets main `+0x2A44` bit 2 and jumps to `0x455F50`. The observer preserves all registers/flags, validates actual length/version/native host identity, then executes this original action even when metadata is invalid. |
| `0x45522E`, `0x45534E`, `0x4553B5` | Alternative carriers read and left alone: fixed-65-byte chat case, `0x06` reply construction and `0x07` readiness-bit store. A chat-plus-start batch depends on the reliable splitter; the raw mode has no splitter. Extending the existing start itself serves both paths without introducing chat metadata. |

The start wire image is packed: code 8 (one byte), version/length (two bytes each),
host native ID (four bytes), match GUID (16 bytes), total 25. The host creates its
GUID at the existing start send. A failed GUID creation sends a same-size zero-ID
start: the stock game action remains possible, contributions refuse it. The
receiver checks native sender equals the declared origin and the bounded source
row's native ID, and requires the source Info host bit. It consumes a pending token
once at the first network B2 capture attempt; load teardown may precede that
capture and therefore preserves the pending token. Every contribution is filtered
by match GUID before binding its origin's stream or sequence.

At attach, `mp_init` verifies the complete start argument/call span, the start dispatch slot/body and length store, plus native player creation and accepted-size branches below. It allocates all four stubs and acquires every page for the five redirects before writing any site; an allocation, byte-match or protection failure leaves all five sites stock. Its writes are those redirects, startup table `0x512AF8`, native CreatePlayer initial data and the original extended start action. It writes no receive output, resource or order field.

Recheck with `objdump -d -M intel` against `pristine/TotalA.exe.pristine`, ranges
`0x490B30..0x490BD0`, `0x496BB0..0x496DB0`, `0x451BC0..0x4520D4`,
`0x461990..0x4619D0`, `0x462710..0x462960`, `0x453D40..0x453ED0`,
`0x45473F..0x45483F` and `0x45522E..0x4553DA`; inspect table slot `0x455F9C`
with `objdump -s`. The splitter and receive-wrapper map above remains applicable.
This does not establish terminal delivery or a complete network recording.

The source-prefix ledger adds no engine patch or field write. Its `advance` call runs at the existing recorder B2 boundary (`0x4954ED`) before capture; GameTime has advanced at `0x4954C0`, so the preceding capture tick can close after all its producer messages. Repeated visits to the same tick do not close it. The existing teardown pre-hook at `0x491B60` calls `tagpu_rec_level_end` before the unit-array cascade; the engine's game-thread state handlers serialize it with capture, as documented in the recorder and `tagpu_reclaim.c:reclaim_teardown_pre`. Final source seals use the copied producer ledger and last captured tick, with no post-teardown engine read. Refused capture aborts instead. These seals name a source prefix, not a peer-clock mapping, recorder application cut or complete file.

The same B2 consumer now passes its actual GameTime to `tagpu_recmp_take_at`, which retains per-source packet consumption metadata in owned channel storage. Teardown's atomic stop publishes its immutable consumption cut; a covering verified source seal and authenticated final-marker receipt govern private-worker close. An explicitly retained consumer can drain proof tails after teardown without changing that cut. These changes add no patch or engine-state write, and provide no post-teardown engine lifetime. Semantic application history and game capture's attachment to the optional multiplayer file context remain open.

The optional recording journal copies local publications and remote consumptions on that same game-thread boundary, with a shared ordinal and a separate application tick. Source envelopes remain unchanged. Its producer pointer is never accessed by the private worker; failed local storage detaches without ending contributions. `RecRing` can transfer its copied context to the world-file writer; the synthetic file fixture validates late proof tails and frozen cuts through that path. Native capture attaches it before any publication/consumption when local recording is enabled. Its game-thread borrow clears before context end permits writer reclamation. This adds no engine read/patch/write beyond the existing capture boundary.

The network adapter now reuses `player_image` at B2 for every local human/hosted AI, rather than reading only resources at player `+0x8C`. It copies the existing normalized 520-byte record/PlayerInfo image. DISASSEMBLED: main's eleven PlayerInfo objects are allocated by `0x463BE0` from `0x41D98B` and retained across reset `0x464508`; destructor `0x463C40` is called only for stack-local records at `0x454538`/`0x454607`. Thus the fixed ten-row bound plus process lifetime, and the existing game-thread capture ordering, authorize this tail read. No probe establishes that lifetime. The recorder's layout keeps the `+0x27` pointer as presence and the `+0x67/+0x6B` block pointers as unit IDs; every other existing normalization remains unchanged.

Bootstrap additionally freezes each active origin's recorder-local row and inclusive global block. It reads `main+0x37EE6` (u16 each), `+0x14351` (u16 slots), `+0x14357/+0x1435B` (array/last-slot pointers) and bounded player `+4`, `+0x73`, `+0x67/+0x6B`. DISASSEMBLED: `0x4854A0` allocates `10*each+1` unit records; `0x485657..0x4856C0` sorts network players by native ID and `0x4858A6..0x4858E0` assigns each rank's fixed block, so rank differs from local row. A roster requires unique nonzero origins, distinct rows/blocks and exactly each-sized aligned intervals inside that allocation. A normalized source player image must retain its own block IDs; remote queue authorization uses the frozen inclusive slots even before a stock create. The cache and delta codec add no byte patch, engine write or new engine address. The optional file context now copies and persists this mapping in `RT_MP_SEATS` (176 bytes), validated against the source roster, world slot/each counts and described seat ownership. The context adds no engine write or byte patch. Network capture creates it through `mp_record_begin` when saving is enabled; mandatory contributions continue with saving off. Complete owner-only unit/effect/Impure authority and native engine playback remain open.

### File authority reconstruction [SOURCE-VERIFIED 2026-10-04]

`tagpu_recmp_history.c` reconstructs copied player/order source history against the frozen roster, without reading engine memory. Separate bounded source and application caches let it validate a proof-tail order completion/player delta while preserving the recorded last complete authority. The writer and C file scan use this semantic validation; a seal over an incomplete order transaction refuses the file. Committed order metadata now includes the source GUID alongside source/application ticks and final sequence; this changes owned cache metadata, not an engine field or the file wire layout.

The reader's `recr_mp_player`/`recr_mp_orders` return authority at its current world tick, rebuilding application prefixes after seek or index/segment changes. Its separate owned contribution payload preserves a partly read world block's lifetime, and each reread block header must match the bounded index and its checksum before allocation. It restores the file cursor afterwards. Unknown stays unknown, and proof records never enter this application cursor. The 2851-byte controlled world-file fixture validates four synthetic player images, a complete nonempty list, a partial 46-node replacement completed only in the proof tail, and exact seek/provenance. Eighteen body mutations with valid rebuilt source seals/cuts refuse in C/Python. This adds no engine address, byte patch or engine write; `RecWorld`/engine authority merging and actual network recording remain open.

### Native player metadata and resource-message carriers [DISASSEMBLED 2026-10-04]

These reads support the isolated-recorder transport's source binding. The work
branch extends native CreatePlayer's initial data and its accepted size; resource
messages remain unchanged. The production adapter and native fixture are described in the
[multiplayer slice](../tadr-port/demo-recorder-multiplayer-slice.html#isolated-native-adapter-and-source-binding-fixture).

| Address | Established fact |
|---|---|
| `0x4CA6A0..0x4CA77F` | Native player creation wrapper, stdcall `ret 0x1C`. It takes the interface from context `+0x4C5`, constructs a 16-byte DPNAME with flags zero, copies the two names into context `+0x11/+0x21` through `0x4E4760`, and supplies 21 bytes of initial player data. The first sixteen bytes are copied from its argument; byte `+0x10` is zero and WORDs `+0x11/+0x13` come from arguments. Native CreatePlayer, vtable `+0x18`, is called at `0x4CA762`, event NULL and flags zero. The sole direct E8 caller in retail `.text` is `0x451421`; a native ownership record is not unused recorder storage. |
| `0x4513A0..0x45143D`, `0x451421` | Read caller span: PlayerInfo receives values from main `+0x37F1B/+0x37F1F` into `+0x8B/+0x8D`, and main `+1/+2` into `+0xA7/+0xA8`; the call passes main `+0x14` as network context. This partial span does not establish every argument's semantic name. |
| `0x4C9D30..0x4C9D73` | SetPlayerData wrapper, stdcall `ret 0x10`: context `+0x4C5`, vtable `+0x74` at `0x4C9D5F`, flags 2 (`DPSET_GUARANTEED`). A NULL interface or nonzero HRESULT returns 0; zero returns 1. A whole-`.text` E8/E9 scan found no direct callers; it proves no indirect-call absence. |
| `0x4C9D80..0x4C9DC3` | GetPlayerData wrapper, stdcall `ret 0x10`: the same interface, vtable `+0x50` at `0x4C9DAF`, flags 0 (`DPGET_REMOTE`), and the same HRESULT-to-BOOL result. No direct E8/E9 caller in the sweep. Adjacent `0x4C9CE0..0x4C9D2D` and the start of `0x4C9DD0..0x4C9E40` were also read; the latter uses `0x4D83B0/0x4D85A0` for session-descriptor storage. Their complete indirect ownership paths were not established here. |
| `0x453D90..0x453F50`, `0x45413F..0x454425` | Read system dispatch and native-create case. Source 0 selects system handling; source/destination IDs are mapped through ten-row scans. In the create case the native ID is message `+8`, initial-data pointer `+0x10`, and size `+0x14`. Retail `0x454315` requires **exactly 21 bytes** before branching to `0x45435B`; another size can reject the joined row through `0x453010` with reason 8. The work branch redirects these six bytes to accept exactly 21 or 301; other sizes resume the original rejection at `0x45431B`. The accepted branch still tests initial-data WORDs `+0x11 == 0` and `+0x13 == 0x50` at `0x45435B/0x454362`. When its password branch is armed, `0x454381` compares the unchanged prefix with main `+0x2BE3` through `0x4F8A70`. Calls `0x450A10`, `0x44FFD0`, `0x44FE40` and `0x456030` were read as part of this case; no new names or guarantees about their unread paths follow. |
| `0x4CA749..0x4CA767` | The 30-byte argument/call span is byte-matched before patching. The redirect at `0x4CA762` replaces `call [ecx+0x18]; xor ecx,ecx` with a stub calling `mp_create_player` on the seven already-pushed stdcall arguments. That helper preserves the original 21 bytes and appends a fixed 280-byte public-key record, or forwards the original arguments when key creation fails. The stub restores `xor ecx,ecx` before resuming at `0x4CA767`; the original `test eax,eax; sete cl; mov eax,ecx` still converts HRESULT zero to BOOL one. The stdcall helper consumes the same 28 argument bytes as the original COM method. |
| `0x4573D0..0x457539` | Existing 58-byte `0x28` sender (the economy map below owns its fields). Both native-source-preserving targeted sends go through `0x451BC0`: the explicit-target call is `0x4574BC`, the bounded ten-row remote-destination loop calls at `0x457515`. The packet starts at stack `+0x0C`; each call pushes 58. Its direct E8 callers are exactly `0x420524`, `0x457380`, `0x4576E5` and `0x4655A1` in the retail sweep. |
| `0x46555F..0x4655B5` | The row index BL must equal main `+0x2A43` before the minimap/radar work and network mode test. Counter `0x51E53C` increments at `0x46558D..0x465595`; every fourth increment reaches `0x4655A1`, with `(EDI,0,0)`. This is a **viewed-record** resource broadcast, not a periodic sender for every hosted AI. |
| `0x4204E0..0x420555`, `0x457300..0x4573C4`, `0x457640..0x457702` | Read additional sender call sites. `0x420524` uses the controlled row (main `+0x2A42`) twice with target/flag zero. `0x457380` lies in resource-exchange row loops ending with delay root `0x4B6B50(250)`, with `+0x11E/+0x129/+0x134` condition tests; the earlier function entry was not read here. The receiver's flag branch iterates active local type-1/2 rows, emits `0x29` through `0x4576B5`, and conditionally sends `0x28` with flag 1 at `0x4576E5`. These branches do not establish a guaranteed post-start advertisement for idle hosted AI. |
| `0x4558FE..0x455909`, `0x45601C` | Resource dispatch-table entry points to `0x4558FE`. It loads its message from stack `+0x10`, pushes source EDI and that message, calls `0x457540` at `0x455904`, then exits through `0x455F50`. Nearby entries `0x456018/0x456020` and cases `0x4558D0/0x45590E` were read only to locate it; no new patch follows. |
| `0x4522DA..0x4522E4`, `0x512B78` | Packet initializer's complete ten-byte instruction is `C7 05 78 2B 51 00 3A 00 00 00`: fixed code-`0x28` length 58. Its immediate begins at `0x4522E0`. A longer sender must change the initializer as well as preserve the original resource prefix; neither change is installed. |
| `0x457D30..0x457FC0`, `0x49563A` | Auto-share root, one direct caller `0x49563A`, not the resource-message constructor. Read prefix tests the network bit, GameTime `+0x38A47` modulo 60, PlayerInfo `+0x97` share bits and the player's resource/share thresholds, then uses `0x464C60/0x464B30`. A later modulo-450 branch begins in the read range; the full remainder and constant `0x4FD4B8` were not interpreted here. |

Recheck these ranges with `objdump -d -M intel` against
`pristine/TotalA.exe.pristine`; use `objdump -s` for `0x456018..0x456024`.
The direct-call counts come from relative E8/E9 targets across the retail `.text`
section, not from a claim about computed calls. SDK player-data ownership and
native Send-from-remote refusal do not establish a periodic AI advertisement.
The installed initial-data extension gives each native human/hosted-AI ID its
owner's public key before play. The private worker's signature covers that origin,
stream, match, private session and actual private sender. Keys come from native
GetPlayerData for the primary ID, not from a private packet. The game thread also calls native GetPlayerCaps at vtable `+0x4C` to check local ownership, GetPlayerData at `+0x50` for each actual primary key, and GetPlayerAddress at `+0x48` for the validated start host ID. Those offsets are the DirectPlay2 SDK interface layout; the native game verifies the calls on TA's existing interface. The bounded 4096-byte compound address, ten-actor roster, match and host role are copied before starting the private worker. Its backend derives the TCP/IP provider through native EnumAddress; no additional engine provider field is read. No primary interface or engine pointer survives in worker storage.

### Native TCP/IP provider address reads [DISASSEMBLED 2026-10-05]

These are the native DirectPlay files installed by `tools/dpinstall.sh`, not
addresses in `TotalA.exe`. `tools/dpport.py` verifies their masked file hashes
before patching its seven discovery-port immediates. The reads support startup
diagnosis; they introduce no provider or engine patch.

| Native binary/range | Established fact |
|---|---|
| `dpwsockx.dll`, `0x5DF075C4..0x5DF0765F` | Address callback compares the GUIDs at `0x5DF01394`, `0x5DF01384` and `0x5DF01374`. Their bytes match SDK `DPAID_INet`, `DPAID_INetW` and `DPAID_INetPort`. ANSI input of at most 128 bytes is copied in full; a larger input is truncated to 127 bytes. The Unicode branch uses conversion root `0x5DF099BF`; the port branch copies a WORD into provider context `+0xF0`. No recorder callback uses this provider context. |
| `dpwsockx.dll`, `0x5DF08260..0x5DF083A0` | Read setup span: a nonzero context `+0xF0` is passed through helper `0x5DF0C902` at `0x5DF082E5`; that helper jumps through `0x5DF011F0`, WSOCK32 ordinal 9. Zero uses the `0x08BA` immediate at `0x5DF082FC`, the site `dpport.py` changes. The address/task path calls `0x5DF08FB3`, `0x5DF0621A` and `0x5DF0776E`. This partial span alone does not name the complete setup function or prove why Open failed. |
| `dpwsockx.dll`, `0x5DF07578..0x5DF079FF` | Additional read spans cover the address callback, table allocation at `0x5DF07667`, task setup at `0x5DF0776E`, worker body at `0x5DF0784E` and thread/event creation at `0x5DF0796A`. The latter returns `80004005` when its event or thread creation fails; other read paths also return that same HRESULT. A generic SDK error cannot identify one of these paths without a runtime observation. |
| `dplaysvr.exe`, `0x01002260..0x01002430` | Read name-server setup span includes the two `push 0x08BA` discovery-port sites at `0x010022FD` and `0x010023E2`, already covered by `dpport.py`. No new claim about the earlier function entry or the entire server lifetime follows from this span. |

MEASURED on the reference setup: a current coverage-build TA host successfully
opens and binds its private session while the primary session is live. Its Wine
Winsock trace shows an exclusive TCP bind at 2300 and 2301 fail with status
`C0000043`, then 2302 succeed; the socket closes and a socket with reuse enabled
binds/listens on 2302. Thus an occupied candidate port occurs on a successful
startup. This is one provider trace, not proof of a race or a fix for the earlier
`80004005` creation/enumeration failures.

### Player-row coordinates across multiplayer peers [DISASSEMBLED 2026-10-04]

The recorder's player-ID normalization (`tagpu_recorder.c`, `id_of`, `RL_T_PLAYER`) is the local engine row plus one. `player_image` also retains the five eleven-byte player vectors and own row byte. Its existing pointer/mask normalization does not convert these local coordinates into another peer's rows. Origin and fixed unit-block identity must establish that conversion before contributed images are applied.

The player/unit-record map above is reconfirmed by aligned retail disassembly. Reset `0x4644D0` is called at `0x42734F`, `0x427D73` and `0x4518AC`. The activation range belongs to `0x464290`, with the fill/join/setup callers listed above. Resource receiver `0x457540` is called by dispatcher `0x455904`; unit array initialization `0x4854A0` by level initialization at `0x4918D4`.

| addresses read | record fields and evidence |
|---|---|
| `0x4644D0..0x4646FA`, player reset | Reads the main pointer at `0x511DE8`; loops rows at `main+0x1B63`, stride `0x14B`, including row 10 (`0x4646D0`/`0x4646D3`). The initial clear retains each `+0x27` PlayerInfo pointer. It clears eleven bytes each at `+0x108` and `+0x113`, then sets each row's own element (`0x4645B3`, `0x4645BB`). `0x4646B4` writes self row `+0x146` to 10 for a free record. At that store the already-advanced `ebp` makes displacement `-0x118` equal the previous row's `+0x146`. The reset's name-format calls at `0x464599` and `0x4645AE` target `0x4E42B0`; this mapping does not change names or PlayerInfo. |
| `0x46434D..0x464381`, player activation | Stores self row at `+0x146`, scoreboard rank at `+0x148` and start position at `+0x147`. Those last two happen to share an initial value; their later writers in the player-record table show different meanings. Only self row is a player reference. |
| `0x45590E..0x455938`, dispatcher statistics flags | Message bytes select writes at `0x45591D` and `0x455930` into `+0x11E` and `+0x134`, indexed by the sender row. These are branches inside the dispatcher, not separate callable functions. |
| `0x457682..0x4576B5`, in `0x457540` | Reads the sending player's `+0x146`, writes the receiving record's indexed `+0x129` at `0x45768A`, and reads indexed `+0x11E` at `0x45769C`. The response uses native IDs from each player's `+0x04` through sender `0x451BC0`. |
| `0x4858BD..0x48592E`, in array initialization `0x4854A0` | Freezes each player's inclusive unit-block endpoints `+0x67`/`+0x6B` and slot bounds `+0x6F`/`+0x71`; its inner loop assigns unit `+0x96` to that player at `0x485902`, then copies player `+0x146` to unit `+0xFF` at `0x485908`/`0x48590E`. The inclusive last-pointer compare is at `0x485923`/`0x485925`; ten player blocks at `0x48592B`/`0x48592E`. |

`tagpu_recmp_seat_map` operates on copied rosters, requiring the same origin set and global block endpoints and a bijection of the ten active-or-vacant rows plus fixed dummy row 10. Active rows match by origin; vacant rows pair in ascending order so unused vector bytes remain reversible. `tagpu_recmp_player_map` first validates the source self row and normalized block IDs, maps the five vectors and self row in a copied 520-byte image, and retains every other byte. The Wine fixture passes 640 roster permutations and 3,520 exact inverse image mappings, including 1–10 actors, shuffled local seats, reversed actor order, preserved vacant/dummy entries, in-place copies, refusal canaries and guarded spans. This is utility verification, not a native multiplayer replay result.

Wire version 2 carries each origin's copied 176-byte source roster as kind 7 at source sequence 1, before typed player/unit/order state. The game adapter validates raw images against that frozen map without engine writes. File history validates all-source and applied maps separately; reader player queries permute the five vectors and self row into recorder coordinates while retaining source provenance. Raw delta baselines stay in source coordinates. A proof-only map/full image validates but supplies no applied map or player. The independent Python decoder and actual C writer/readers verify all eleven vector entries through synthetic playback/seek. The source advertisement copies the bootstrap roster already bounded above. Projectile `+0x66`, explored bits and declared Impure player IDs still require separate integration/audit work.

**Copied unit coordinates and owner authority.** `tagpu_recmp_unit_map` requires the frozen origin to own the slot's global block, normalized owner ID `+0x96` to equal source row + 1, and owner byte `+0xFF` to equal the source row. It maps those fields and nullable resource player ID `+0xEC` (0..11, zero retained), plus attacker row `+0xF4` (0..10, dummy ten retained). Slot zero is outside every owner block: its `+0xFF = 255` sentinel cannot authorize an ordinary unit. Every other byte of the 328-byte normalized record/mover image is retained. The isolated Wine fixture passes 640 roster permutations and 464,640 byte-exact inverse mappings, including every bounded attacker/resource-player value, in-place copies and unchanged-output refusals. These utilities touch no engine allocation.

| addresses read for unit-coordinate audit | ownership, callers and evidence |
|---|---|
| `0x485A40`, unit create | Sets alive bit 28 at `0x485A7B`, clears attacker pointer at `0x485C7D` and writes attacker row ten at `0x485C83`. Reads the fixed owner byte at `0x485CF7` and passes it to resource initializer `0x401070` at `0x485D0A`. The create callers and owned objects are listed in the record map above. |
| `0x401070(row)`, thiscall, `ret 4` | Zeroes thirteen resource-object dwords (0x34 bytes), masks the argument to a byte at `0x401089`, and writes `main+0x1B63+row*0x14B` to object `+0x30` at `0x4010A3`. There is **no ten-row bound** in this initializer. Direct callers are player resource initialization `0x4647EA` and unit create `0x485D0A`. In a unit this pointer is record `+0xEC`, not its owner pointer `+0x96`. Equality in the earlier live sample is a measurement, not the codec's safety argument. |
| `0x489CE0`, damage | Reads attacker unit `+0xFF` at `0x489DB4` and writes victim attacker row `+0xF4` at `0x489DC0`; its damage callers are listed above. Death calls `0x44FE40` at `0x486782` and stores the returned row at `0x486787`. |
| `0x44FE40(netid)`, `ret 4` | Returns dummy row ten for −1 or no active matching native ID. Direct `.text` callers: `0x451BFF`, `0x451C62`, `0x452999`, `0x4529C0`, `0x4529CE`, `0x452CFF`, `0x453057`, `0x453F74`, `0x45418C`, `0x454485`, `0x4546E8`, `0x486782`, `0x493576`. The ten-row scan is described below. |
| `0x44FEB0(pointer)`, `[INFERRED] player-pointer-to-row`, `ret 4` | NULL returns byte ten at `0x44FEB8`; otherwise reads byte `pointer+0x146` at `0x44FEBD`. A complete retail `.text` sweep finds no direct caller, and the binary contains no four-byte literal occurrence of its address. This does not establish indirect unreachability. The contribution mapper does not call it. |
| `0x44FED0(netid)`, `[INFERRED] transport-ID-to-player-record`, `ret 4` | Scans ten rows with active controller `+0x73` and native ID `+4` (`0x44FEED..0x44FF2E`). An initial miss returns NULL at `0x44FF36..0x44FF3B`. A match takes `0x44FFBC` back to `0x44FF32` and performs a **second** active-ID scan (`0x44FF4E..0x44FF8B`) before constructing `main+0x1B63+row*0x14B` at `0x44FFB2`. A second-scan miss uses dummy row ten (`0x44FF95`). Direct callers: `0x46DA37`, `0x46E02E`, `0x46E0D0`. The contribution mapper does not call it. |

Wire kind 8 carries bounded batches of full/changed-word live images or explicit absence. The game adapter reads only a frozen local type-1/2 owner's bounded slots and follows a mover only after the live owner pointer agrees with that actor. Birth comes from the already declared `tagpu_patches.c:s_hitStamp` table; `HIT_UNKNOWN` is refused for live units. A cleared alive bit permits an absence record with birth metadata zero when the raw stamp is unknown: this neither changes nor records the underlying Impure table. Complete declared-state authority remains outstanding. The cache validates the whole copied packet and source-to-destination allocation agreement before any slot publication, including tombstones. Source and applied file caches remain separate; proof-only full/delta/absence establishes no applied unit. The four-actor native test verifies all four first full unit-image source checksums/provenance, human owner-row remapping, and ordinary-exit cuts/receipts. Reader queries reconstruct copied unit authority through synthetic seek; engine application, owned COB/model/effect objects and full Impure state remain open. Native files retain these copied contributions separately from the observed world. No byte patch or engine write is added by this carrier.

### The network receivers take a unit index straight off the wire — `0x09`, `0x0B`, `0x0C`, `0x2C` [DISASSEMBLED 2026-09-25]

Landing B3 of [the port's section B](../tadr-port/sim-fixes.md). The dispatcher `0x45486D` indexes the
jump table `0x455F84` by `code − 2` and gives each case the sender's player record in `edi` and the
record pointer on the stack. Five cases index the unit array — begin `*(main+0x14357)`, last
inclusive `*(main+0x1435B)`, stride `0x118` — by a `u16` from the record with no bound, and one also
by a signed delta, an unbounded type, and a signed round-robin remainder (the heading keeps the
four this section first listed, since other pages link to it):

| code | case | handler | the unbounded reads |
|---|---|---|---|
| `0x09` | `0x4553DA` (→ `0x4553E9`) | `CreateFromNetwork 0x4861D0(player [esp+0x14], rec)` `ret 8` | index `rec+3` (0 faults at `0x486237`), type `rec+1` (indexes the def table `main+0x1439B + type·0x249` at `0x48626F`) |
| `0x0B` | `0x45540D` | `0x489CE0(rec)` `ret 4` | victim `rec+1`, attacker `rec+3` (`0x489CED..0x489D36`); a heal-kind writes HP at `0x489D80` |
| `0x0C` | `0x45541C` | `0x4866D0(rec, 0)` `ret 8` | index `rec+1` (0 faults at `0x486706`), killer `rec+7` (`0x486753..0x486777`; 0 → NULL, handled) |
| `0x2C` | `0x4553EE` | `0x48B920(player edi, pkt)` `ret 8` | dirty-entry delta (signed, `0x48B985`), type (`0x48B9AD`), the mover after a refused create (`0x48BA05`, TADR's 13 field faults at `0x48BA07`), the round-robin type (`0x48B40E`) and model object (`0x48B49C`), and the **signed** `idiv` remainder at `0x48BAAB`; the bit reader has no end, and the message's own `[16]` size is read and discarded (`0x48B944`). Two unit references ride inside it: a `0x4FD9E0`-class move payload's target (`0x44E0D0`, scaled at `0x44E0DE..0x44E0F9`) and the round robin's carrier (`0x48B56B`, scaled inside `0x48AB70` at `0x48ABAF..0x48ABC4`) — below |
| `0x0A` | `0x4553FE` (→ `0x455408`) | `0x48AB70(rec)` `ret 4`, [attach and detach](units.md#0x48ab70-attach-and-detach-one-unit-to-another-mapped-by-us) | child `rec+1` (`0x48AB8A..0x48AB9F`), parent `rec+3` (`0x48ABAF..0x48ABC4`), each 0 → NULL, and a NULL child returns having done nothing (`0x48ABC7`) while a NULL parent is the detach (`0x48ABA9`); the child is then read at `+0x110` (`0x48ABCF`) and the parent at `+0x110` (`0x48ABFD`) and written at `+0x8A` (`0x48AC82`). The sender, the wrapper `0x48AAC0`, writes its child's own `+0xA8` (it reads the child's `+0x110` at `0x48AAC7` first, so the child is never NULL) and its parent's or 0 |

**The player block** each `0x2C` path trusts: `[player+0x67]` = first slot, `[player+0x6B]` = last, and
`10·[main+0x37EE6]+1` slots with slot 0 a sentinel. The block is assigned at `0x4858A6..0x4858E0` as
`first = begin + (1 + k·N)·0x118`, `last = first + (N−1)·0x118`; that is the shape the entry check
`0x48B960` verifies (a wild `[player+0x67]` fails it even where a 16-bit delta could not). `k` is a
**rank**, not the record's index: the blocks are handed out in the order of an insertion sort
(`0x485657..0x4856C0`) that, in a network game, compares the records' DirectPlay ids `[rec+4]`
(DISASSEMBLED). Confirmed live 2026-09-25 in a four-player game with an AI seat: blocks at slots 1,
1501, 3001, 4501 and 6001, `N` 1500, and on one joiner record 2 held the block at 6001.

DISASSEMBLED 2026-10-05 for offline owner reconstruction: `0x48566C` calls
`0x435100`; only result 3 takes the ID comparison (`0x485671..0x48567E`).
Other modes compare record pointers at `0x485680`. Changing native IDs in an
offline skirmish therefore cannot recreate the recorded native block order.
The block initializer writes player first/last pointers `+0x67`/`+0x6B` at
`0x4858BD`/`0x4858E0`, copies those units' global `+0xA8` IDs into player
`+0x6F`/`+0x71` at `0x4858ED`/`0x4858F8`, then loops inclusively through the
block (`0x4858FC..0x485925`), fixing unit owner pointer `+0x96` at `0x485902`,
owner row `+0xFF` at `0x48590E` and group `+0xAC=-1` at `0x485914`.
Sentinel slot zero gets owner `NULL`/row 255 at `0x485880`/`0x485887` and is
outside the loop. The insertion fallback calls comparator `0x485940` at
`0x485691`. Its body (`0x485940..0x485974`) calls `0x435100`, the three-byte
`mov eax,[ecx]; ret` mode getter, then returns unsigned less-than on native
IDs for mode 3 or record pointers otherwise. The insertion helper `0x488920`
calls that comparator at `0x488938` and `0x48894C`, shifts pointers backwards
and returns with `ret 0xC` at `0x488958`. The minimum-element invariant and the
other sort comparisons are in *Replay player names and unit-block ordering*.

**Allocation ownership boundary [DISASSEMBLED + SOURCE + MEASURED 2026-10-05].**
Both sort branches reach `0x48587E`; the fixed ten-row branch jumps there at
`0x4856C2`. Its stack holds the freshly allocated unit base at `esp+0x14`
and exactly ten sorted player pointers at `esp+0x18..+0x3F`. The array is
zeroed by `0x485554..0x48555E`; the global slot word `+0xA8` is assigned at
`0x485603` and definition-table pointer `+0x92` at `0x485617`, before sorting.
`0x48587E` clears EDI, `0x485880` sets the sentinel owner row to 255 and
`0x485887` sets its owner pointer to NULL. `0x48588D` then points EBP at the
stack's player table and `0x485894..0x48592E` initializes all ten paired blocks.
The native allocator's three named allocation calls are at
`0x485515`, `0x48557D` and `0x48559C`, all targeting `0x4D83B0` with two
caller-popped arguments. This work reads their call contract, not the allocator's
complete heap implementation.

`tagpu_replay.c:mp_arena_order` observes the nine nonrelative bytes at
`0x48587E` (XOR EDI and the whole sentinel-row MOV). A multiplayer puppet
reorders only the bounded stack permutation according to frozen actor global
blocks; vacant rows and ranks pair in ascending order. The native loop remains
the sole initializer of all paired player/unit ownership fields. Counts and the
fresh arena base must agree with the recording before the stack changes. A
refusal stops application before any recorded unit is constructed. The seating of a
mode-3 recording arms it, and that recording's first step refuses to apply units
unless it reported READY. Other levels leave the stack unchanged.

**The allocator run outside the game** [DISASSEMBLED + MEASURED 2026-10-05]
(`tools/replay/recmp_arena_test.c`, `recmp_arena_test.py`). The test loads the retail
image into its own process under Wine and runs `0x4854A0..0x48593B` (the allocator,
whose plain `ret` is `0x48593B`) with the comparator `0x485940..0x485974`, retargeting
24 absolute operands: the 21 four-byte reads of the main pointer `0x511DE8` in that span,
each after an `A1` or an `8B` with a disp32 ModRM (`0x4854A4`, `0x4854B5`, `0x4854CA`,
`0x4854DD`, `0x4854F7`, `0x48551C`, `0x48552C`, `0x485562`, `0x485584`, `0x4855A3`,
`0x4855B2`, `0x4855E0`, `0x48560C`, `0x48561E`, `0x48562D`, `0x48565C`, `0x485794`,
`0x48582D`, `0x485896`, `0x4858C2`, `0x485941`), and the three `push 0x485940`
operands naming the comparator (`0x48568B`, `0x485744`, `0x485765`). A byte scan of
`0x4854A0..0x485974` finds those 21 and those 3 and no other occurrence of either value.
Relative calls keep their displacements, since the whole image moves together; the
allocation call `0x4D83B0` is the one dependency substituted. MEASURED: 206 reordered
allocations and two stock-order controls preserve every global slot and all ten blocks.

**Thread handoff.** This allocator runs on the loader thread, through
`0x497180` → `0x497581` → `0x4917D0` → `0x4918D4` → `0x4854A0`.
The Start's seat fill precedes the loader-start call `0x4B6B20` at `0x4982CA`,
which takes entry `0x497C70` (the CRT thread wrapper), not the allocator itself.
The callback consumes an immutable copied roster published at seating with
`InterlockedExchange`, claims it with `InterlockedCompareExchange` and publishes
READY or FAILED through another interlocked operation. It neither reads the
mutable reader nor writes the game-thread failure flag. At the first step,
`first_checks` requires READY before construction. The engine reaches that step
only after its loading-state test of `main+0x38D75` bit 1 at
`0x498342..0x49834D`; the loader's final store sets that bit at `0x497C62`,
after the allocator and its callback have returned. Start undo precedes thread
creation, and replay teardown follows that load-done gate, so the copied plan
cannot be reset while this loader still consumes it. The owning thread and the
publication/result ordering are separate facts; a fast load proves neither.

`tools/replay/recmp_arena_test.py` executes the retail allocator, comparator and
insertion helper in a relocated image inside a standalone Wine fixture. Its
24 verified absolute operands (21 main-pointer reads and three comparator PUSH
arguments) are retargeted; all relative calls retain their original displacement.
Only the three heap calls use a fixture allocator. It does not call the exe
entry point or launch a game. Two untouched controls reproduce pointer-order
offline blocks and native-ID-order multiplayer blocks; 206 reordered allocations
then verify all ten first/last pointers and IDs, every unit owner pointer/row,
group -1, preserved global slot ID and the sentinel. Actors cover one through
ten, including noncontiguous active blocks; each values 1, 20, 1,500 and 6,553
exercise the copied bounds. The latter is a format-bound control, not a claim
that the game permits that unit limit. 640 copied permutations, overlapping
input/output and twelve unchanged-output refusals also pass. The production hook
is installed and inert through all four processes of the coverage skirmish
regression. Multiplayer loader admission and native owner-unit reconstruction
remain unverified.

**Which thread.** The receivers above run on the game thread only, by the dispatcher's gate:
`0x451FD0` (called at `0x491369`) is the only filler of the receive-mask table `0x512BC0` for these
codes, and gives `0x09`, `0x0A`..`0x10` and `0x2C` the mask 4 (`0x451FE7..0x452017`, one store each;
`0x0E`'s is `0x45200B`). The dispatcher passes a code in net state 5 only with mask bit 1
(`0x454745..0x454760`), in state 6 only with bit 2 (`0x454762..0x45477D`), and in any other state only
with bit 0 (`0x454791..0x4547A7`); a code whose bit fails in state 5 or 6 goes straight to `0x455F50`
(`0x45477F..0x45478B`, the `je` at `0x45478B` state 6's), never on to the bit-0 test. State 6 is set at
`0x498445`, on the game thread after the load (state 5 during it).

**One receive, one message.** In retail, the pump `0x453D40` calls `0x4534E0` per message (`0x453D94`), which
receives into the buffer at `main+0x2A38` — through the transport `0x462F30` when `[0x506DBC]` is set
(the image's 1), or straight from DirectPlay (`0x4C9840`, `IDirectPlay::Receive`) when a `-p` switch
below 0 has cleared it (`0x49F23F` → `0x4578D0` → `0x4619EB`). A too-small buffer is grown and the receive retried
(`0x453565`, `0x4535EE`). The delivered length is handed only to the statistics call `0x415EF0` (code,
length, 0; at `0x453595` and `0x45361F`) and then dropped: the pushes before each call,
`0x453585..0x453594` (transport) and `0x45360F..0x45361E` (raw), take the length from the local
`[esp]` and the code from the buffer's first byte, so the spans B3 compares before replacing the calls
are `0x453585..0x453599` and `0x45360F..0x453623`. The transport's splitter `0x463790` walks a
packet after its 4-byte header, taking each message's length from the table at `0x512AD8` by code —
except a `0x2C`, whose length is its own `[16]` size, read at `0x46391F`/`0x4639E5` — and stops at a
message that does not fit what is left (`0x46393E`, `0x463B3B`), so on the transport a delivered
`0x2C`'s length **is** its size field. A code outside `[2, 0x2C]` ends the walk (`0x4638F2..0x4638FC`).
The pump takes its message pointer once, before its loop (`0x453D90` → `[esp+0x10]`; the back edge
`0x455F59` re-enters at the call `0x453D94`, which returns to `0x453D99`, `test eax,eax`, the result kept
at `[esp+0xF0]` for `0x455F50` to test — the return address B3's length capture checks before it
writes the pump's `[esp+0x10]`), and the dispatcher's cases read the message there
(`0x4553DA`, `0x4553EE`, `0x4553FE`, …; one, `0x45504A`, builds a `0x1C` reply in the buffer itself);
no other instruction of the pump writes that slot or takes its address. The growth is `0x4D84A0(block, "PACKET DATA AGAIN", size)` → `0x4D84C0`, a realloc
[INFERRED: the block and size in, the block out; `0x4E9020(block, size)` inside], so a growth that moves
the block inside one pump call leaves every later message of the call dispatched from the freed
block (DISASSEMBLED). `main+0x2A34` (the capacity) and `main+0x2A38` (the buffer) have no writer by
displacement other than the two growth paths, yet hold 0x2000 bytes at the main menu before any
session, so something of another form sets them; MEASURED 2026-09-25, they stayed 0x2000 at one
address through a four-peer tier-2 game on every peer. On the transport no well-formed message
comes near that: the splitter's longest fixed length is 186 (code `0x20`) and a `0x2C` is capped by
its sender, so a growth — and the stale pointer — takes a malformed message. Nothing reads a message after its case returns: each
case jumps to `0x455F50` (the case calls return to `0x4553E9` for `0x09`, `0x4553F9` for `0x2C`,
`0x455408` for `0x0A`, `0x455417` for `0x0B` and `0x455428` for `0x0C`, each a `jmp 0x455F50`), and
the next message is a new receive. A length of 0 keeps the splitter's
counting loop (`0x4638F0..0x463947`) on the same message for good whatever the code (`sub edi,eax`
with eax 0, then `inc ebp` and back), and its third walk (`0x463AD3..0x463B8B`, entered at
`0x463ABB`) queues the same message until the queue holds 512 (`0x463B46`) (DISASSEMBLED). Both
take the length at one place after the `0x2C` and table paths join: `0x463939` (code at `[esp+0x28]`,
stored `0x4638F4`) and `0x463B33` (code at `[esp+0x2C]`, stored `0x463AD9`); the queuing pass
`0x4639BC` runs the count the counting pass found. Each loop's head loads and stores the code —
`0x4638F0..0x4638F7`, and `0x463AD5..0x463ADC` from the third walk's `mov al,[esi]` at `0x463AD5`
(entered there from `0x463AD1`, and through `0x463AD3` from the back edge) —
the spans B3 compares with the two length points. The counting loop's exit is `0x463949` (`mov
[esp+0x24],ebp`, the count found; `[esp+0x28]`, what is left, at `0x46394D`), where a code outside
`[2, 0x2C]` (`0x4638F8`, `0x4638FC`) and a message that does not fit (`0x463940`) go; the splitter's
`return 1`, `0x463B91` (`pop edi` … `ret 0x18`), is where a count of 0 (`0x463953`) and every end of
the third walk go — a code outside `[2, 0x2C]` (`0x463ADD`, `0x463AE5`), a message that does not fit
(`0x463B3D`), a full queue (`0x463B4C`). B3's refusal of a length ends each walk through those same
exits (`0x463949`; `0x463B91`). The table `0x512AD8` (a `u16` per code, stride 4,
filled at `0x451FD0..0x452311`) read live 2026-09-25: 0 for codes 4 and `0x2B` — which would hang the
counting loop the same way — and at most 186 (code `0x20`) for the rest of `[2, 0x2B]`.

**A wire `0x09` comes from its unit's own player.** The transport sender is the dispatcher's `edi`,
which `CreateFromNetwork` pushes last (`0x4861EC`), so it is `[esp+0]` at `0x4861F7`. MEASURED
2026-09-25 on four peers of a tier-2 game (three humans and an AI seat on the host): all 18 011 wire
creates put the unit in the sender's own block, the player argument always equal to the sender —
including the AI's, which arrive from the AI's own player record, not from the human hosting it.
The pump names that record before the dispatch (DISASSEMBLED): it compares `[main+0x4C9]` [INFERRED:
the id the receive reports as the sender] with the DirectPlay id `[rec+4]` of each of the ten records
whose `[rec+0x73]` is nonzero (`0x453DBD..0x453E12`), keeps the match's index at `[esp+0x14]`
(`0x455F78`), or 10 when none matches or the id is −1 (`0x453E14`, `mov byte [esp+0x14],0xA`), and
takes the record `main+0x1B63 + index·0x14B` (`0x453E84..0x453EA1`) — for a sender it cannot find,
record ten, one past the ten. Ten is the engine's own count of the records: the loop at `0x406E05`
(`mov ebx,0xA`, in the AI plan's `Weight` command `0x406DB0`) steps `0x14B` over them (`0x406E2D`)
and ends after ten (`0x406E33`). B3's `0x09` stub bounds the sender to one of the ten before it reads
the sender's block.

**The `0x2C` stream.** `0x48B920` reads `[8] code, [16] size, [32] GameTime` (the bit reader
`0x415DC0(nbits)`, thiscall `ret 4`, ecx the reader `{u32* buf, u32 dword_idx, u32 bit_idx}`), then a
dirty list of `[16] delta, [typeBits] type` (typeBits `main+0x14393`) and the move class's payload
(the writer ends the list once the message holds 0x200 bytes, a test made after each entry is
written, `0x48B7E5..0x48B7FB`, so the last entry, the terminator and the round-robin entry can take a
message past 0x200), a `[16] 0xFFFF` terminator, and one round-robin full-state entry
for slot `GameTime % N` (`0x48B3F0`). A dirty entry whose type differs from its slot creates the unit
(`0x48BA00 → 0x4861D0`, returning to `0x48BA05`); the round robin creates at `0x48B497` (returning
to `0x48B49C`). A type with no move class (`def+0x22F != 1`, `0x4862CF`) leaves the mover `[esi]` at
0, and `0x48BA05` dereferences it — so the **dirty list** checks the move class, which is also the
sender's own rule (it lists only units with a mover, `0x48B782..0x48B786`). The **round robin**
carries every unit, structures included, and tests the mover itself before its one use (`0x48B6E8`:
`[edi] == 0` skips to `0x48B6FC`), so there a type with no move class is well-formed and only the
type is bounded; its after-create guard is on the model object `[edi+0x9E]` (the Object3do, set at
`0x485DCC`; `+0x9A` is the COB script), which `0x48B4A6` writes through
(DISASSEMBLED; MEASURED 2026-09-25: the AI's structures, types 78 and 112, arrive in the round robin
on every joiner of a four-peer game). GameTime is `main+0x38A47`,
incremented at `0x4954C0`; `UNITINFOCount` is `main+0x1438F` (the type bound `[1, count)`).

Locally none of these indices can be 0 (`0x4864B0` writes the live unit's own `+0xA8`, ≥ 1), so only a
malformed or foreign message reaches them — the fix is stock-exact for every well-formed one and is
[class local](../tadr-port/sim-fixes.md#the-landings). Our stubs are entered by a jmp at a
clean 5-byte boundary (the `0x0C` destructor at `0x4866E5`, after stock's `push edi`), verify the
whole stock span first, and continue at the same address; a misframed `0x2C` points the reader at a
zero dword and jumps to the engine's own end-of-list `0x48BA28`. The `0x2C` is parsed from a copy:
at `0x48B92B` (eax = the message, loaded at `0x48B923`, stored as the reader's buffer at `0x48B933`;
`0x48B933..0x48B93E` sets the reader `{buffer, word 0, bit 0}` at `[esp+0x14..0x1C]`, with `push 8` taken)
the message's length — the receive's delivered length, kept per thread at the two statistics calls,
and the size field, the smaller — is copied into one static DLL buffer (the receiver runs on the
game thread only, by the dispatcher's gate) followed by 48 zero bytes,
and eax is the copy. The reader is then bounded by that length before each read the stubs precede:
the entry (header and first delta), the delta (the type read next), `0x48BA5E` (the flag bit read
directly at `0x48BA71`, and when set the round-robin type read `0x48B409`) and `0x48B40E`. Past the
last check the engine reads at most **229 bits** (DISASSEMBLED, every path): the move-class payload
`[vt+0x24]` then the next delta `0x48BA19` — the vtables `0x4FD458`, `0x4FD980`, `0x4FD9B0` →
`0x44EFD0` (none), `0x4FD488` → `0x44F5C0` (1 + 2 + 3 × 32 = 99, its loop a 2-bit count),
`0x4FD9E0` → `0x490A10` (2 + max(`0x44E080` 184, `0x44E9C0` 209) + 2 = 213), + 16 — and the round
robin's tail after `0x48B40E` at most 211 (`0x48B4A9..0x48B6F2`); none loops on data it reads. The
reader `0x415DC0` also loads the next dword whenever a read reaches the end of one
(`0x415E0C..0x415E3E`; a read from bit 0 of a dword takes it alone, `0x415DF5`), so 29 + 3 + 8 bytes
cover it and the padding is 48.

**Where each stub continues and drops** (DISASSEMBLED 2026-09-25). The span is the stock bytes
compared before any write; the branch sits at its first clean 5-byte boundary.

- **`0x09`**, `CreateFromNetwork`: span `0x4861F7..0x48621F`. Stock stores player·0x14B over its own
  player argument at `0x4861FE` (`mov [esp+0x20],edx`), which the stub repeats, and it continues at
  `0x486220` with `esi` = the unit. `0x486220` reads the create's player's `[rec+0x67]` (`mov
  eax,[ebx+edx+0x1BCA]`, its block's first slot), and `0x486229` (`jne 0x486237`) falls, when that is
  0, into `0x48622B`: `xor eax,eax` and the epilogue, `ret 8` — the engine's own refusal of a create
  for a player with no block, and the stub's drop.
- **`0x0C`**, the destructor `0x4866D0`: span `0x4866E0..0x486705`, from the index read `mov
  ax,[ebx+1]`; stock's `push edi` at `0x4866E4` lies inside it and stays, since the branch is at
  `0x4866E5`. It continues at `0x486706` (the live test on `[esi+0x110]`) with `esi` = the unit and
  drops to `0x486E59`, the destructor's own epilogue (`pop edi` … `ret 8`), which a victim that is not
  live takes (`0x486710`), so the kept `push edi` balances. The killer: span `0x486753..0x486777`,
  always continuing at `0x486778`, `mov [esi+0xF0],eax`, the killer pointer into the victim's `+0xF0`.
- **`0x0B`**, `0x489CE0`: span `0x489CED..0x489D36`, both reads. It continues at `0x489D37` (`test
  esi,esi`: a victim of 0 exits there, a dead one at `0x489D4A`) and drops to `0x489F93`, the
  receiver's epilogue (`pop edi; pop esi; pop ebx; ret 4`), where both of those go. The `0x0B` and
  `0x0C` stubs count a record as the wire's by the dispatcher's return address (`0x455417`,
  `0x455428`): `0x489CE0` is also called by the damage function and `0x0B` sender `0x489BB0`
  (`0x489C89`), and `0x4866D0` by `Send_UnitDeath` (`0x486679`); an E8/E9 scan finds no other callers.
- **`0x2C`**, `0x48B920`: the copy (span `0x48B92B..0x48B932`, continuing at `0x48B933`) and the entry
  (span `0x48B960..0x48B96D`, `mov eax,[edi+0x67]; mov [edi+0x18],ebp; cmp eax,esi; je 0x48BAC8`,
  continuing at `0x48B96E`, the first delta's `push 0x10`) drop to `0x48BAC8`, the receiver's epilogue
  (`pop edi` … `add esp,0x24; ret 8`) — stock's own exit for a player with no block (`0x48B968`), for
  a clear round-robin flag (`0x48BA97`), and after the round robin. The delta (span
  `0x48B985..0x48B99C`) continues at `0x48B99D` (`mov ecx,[eax+0x14393]`, typeBits, for the type read
  at `0x48B9A8`). The type (span `0x48B9AD..0x48B9B5`) continues at `0x48B9B6` for a create — `mov
  dx,[esi+0xA8]`, the start of the `0x09`-shaped record built on the stack for `0x4861D0`, called at
  `0x48BA00` with the slot's `[esi+0xFF]` as the player — or at `0x48BA05` for the slot's own type;
  the after-create check (span `0x48BA05..0x48BA0C`) at `0x48BA0D`, `push eax`, the reader handed to
  the move class's `[vt+0x24]` at `0x48BA10`. The flag's stub replaces `0x48BA5E..0x48BA65` (`mov
  ecx,[esp+0x18]; mov eax,[esp+0x10]`, replayed in the stub) and continues at `0x48BA66` (`mov
  esi,[esp+0x14]`, the reader's dword index, before the bit read at `0x48BA71`), or ends at
  `0x48BAC8`. After the remainder, patched in place at `0x48BA9F..0x48BAAC`, `0x48BAAD` (`mov
  ecx,[edi+0x67]`) scales it from the player's first slot — with the dirty loop and the block sweep
  `0x48BA28`, one of the reads of the block the entry check covers.
- **The round robin** `0x48B3F0`: the type's span is `0x48B40E..0x48B414` (`xor ebp,ebp; cmp ax,bp;
  jne 0x48B43F`). A stop goes to `0x48B435`, the entry's epilogue (`ret 8`); type 0 to `0x48B415`,
  stock's own type-0 path (the slot `[esp+0x38]`: an empty one skips to `0x48B6FC`, an occupied one
  has bit 14 of its `+0x110` set, `0x48B426..0x48B42F`, and returns through `0x48B435`); any other type
  to `0x48B43F` (`mov edi,[esp+0x38]`: the slot's own type goes on at `0x48B49C`, another is created
  at `0x48B44C..0x48B497`). After the create the span is `0x48B49C..0x48B4A1` (`mov eax,[edi+0x9E]`),
  continuing at `0x48B4A2`, `push 0x10`, before `0x48B4A6` writes through `eax`.

**The unit references inside a `0x2C`** (DISASSEMBLED 2026-09-25, every decoder the dirty list and
the round robin reach). Two of the values the stream carries are unit indices that address the array,
each an optional reference whose 0 the engine itself takes as no unit; both are bounded:

- **A move payload's target.** `0x44E080` (thiscall `ret 8`: `this`, a `+0x12` pointer, the
  reader) is the payload parse of the `0x4FD9E0` move class, reached only from `0x490A10` (its
  `[vt+0x24]`, called for a dirty entry at `0x48BA10`) at `0x490A4F` when the 2-bit selector read at
  `0x490A30` is 1. Under flag bit 0 of the 8-bit `[this+8]` it reads `[16]` into `[this+0x10]` and
  then `[16]` the target index (`0x44E0D0`); stock's zero test `0x44E0D5` (`cmp ax,bp`, `ebp` zeroed at
  `0x44E089`) sends index 0 to `0x44E0DA` (`xor eax,eax`), and any other is
  scaled to `first + idx·0x118` with no bound (`0x44E0DE..0x44E0F9`), and either is handed at
  `0x44E0FC..0x44E0FF` to `0x489690`, the reference set (thiscall on the reference at `[this+0x16]`,
  `ret 4`): it unlinks the reference from its old unit's `+0xA2` chain (`0x489690..0x4896BC`), then
  links it into the new unit's — reading `+0xA6` and writing `+0xA2` — or, for NULL (`0x4896C3`) or a
  slot whose `+0xA6` is 0 (`0x4896C7`), clears it: `0x4896E6` zeroes the reference's `+4` and `+8`
  (`0x4896BF..0x4896ED`). The owner's writer `0x44DDC0` sends the target's
  own `+0xA8`, or 0 for none (`0x44DDEC..0x44DDF9`). `[this+0x10]` is not a unit index: the class
  passes it as the piece to `0x43E060(out, unit, piece)` (`0x44E400`), and `0x43DEF0` bounds a piece
  itself; the `0x44E190` constructor sets it `0xFFFF`. **Bounded at `0x44E0DE`** (span `0x44E0D5..0x44E103`,
  from the zero test to the call of `0x489690`, continuing at `0x44E0FC`): an index past the array
  takes `0x44E0DA`'s own NULL, counted (`target`).
- **The round robin's carrier.** The round-robin entry `0x48B3F0` (`ret 8`, called once, at
  `0x48BAC3`) is the full-state reader, `0x48B409..0x48B6F9`. After the type and the create it reads
  `[16]` into `+0x108`, `[8]` ÷ 255 (the float at `0x4FD750`) into `+0x104` when it differs (MEASURED:
  the refresh a non-owner's copy took 26–37 s after a create at 1500, which the kill count reads —
  *The kill count reads this peer's copy of the victim*, below), `[8]` a state mask handed to
  `UNITS_SetStateMask 0x48B090` (`0x48B50E`, `0x48B519`), `[2]` into `ebx` and one flag bit read in
  place (`0x48B527..0x48B557`). With the flag set it takes the child id, its own `+0xA8` (`0x48B55B`),
  reads a **15-bit carrier index** (`0x48B56B`) and
  an 8-bit attach point (`0x415E60`, `0x48B579`) and applies the attach record `{+1 the unit's own
  +0xA8, +3 the carrier, +5 the point, +6 ebx}` through `0x48AB70` (`0x48B58B`), which scales the
  carrier with no bound; with it clear, a unit whose `+0x86` is set is detached (`0x48B59A..0x48B5C5`,
  parent 0 written at `0x48B5B5`, point `0xFF`). The owner's writer sets the flag exactly when its unit
  has a carrier and sends the carrier's own `+0xA8` in 15 bits — `0x48B300..0x48B321`: the carrier
  `[unit+0x86]`, 0 for none, else its `+0xA8`, written by `0x415C10` at `0x48B321` — and the unit's own
  `+0xF9` in 8 (`0x48B2A4..0x48B332`). The flag-set
  path returns after the attach (`0x48B590`); the flag-clear path goes on to `[32] × 3`, the
  position, into `+0x6A` (read `0x48B5CA..0x48B5E6`; stored `0x48B676..0x48B682` when its grid
  cell `+0x76`/`+0x78`, recomputed from the new position, and `+0x110`'s low two bits are
  unchanged (`0x48B64B..0x48B66C`), else `0x48B684..0x48B6A7` between the grid's remove
  `0x47D0E0` and re-stamp `0x47CC30`, then `0x4827B0` — so the round robin corrects a copy's
  position within N of the owner's ticks), `[16] × 3`, the body turn, into `+0x64`
  (`0x48B5EB..0x48B60F`, stored `0x48B6C4..0x48B6E4`) and `[32]` into the mover's `+0x20`
  (`0x48B6F2`) — all into the entry's own slot. **Bounded at `0x48B574`** (the record's
  `mov [esp+0x17],ax`; span `0x48B55B..0x48B58F`, from the child id to the call of `0x48AB70`,
  continuing at `0x48B579`): a carrier past the array becomes 0, `0x48AB70`'s own no parent
  (`0x48ABA9`), counted (`carrier`).
- **The kill count reads this peer's copy of the victim** [DISASSEMBLED + MEASURED 2026-09-26].
  The destructor runs on every peer and counts a kill only when that peer's copy of the victim
  reads `+0x104` = 0.0 (the gates, and the three-peer measurement:
  [its entry](memory.md#0x4866d0-the-unit-destructor-inferred-name-units_destroy), *Who counts a
  kill*). A non-owner's copy made by a create keeps its `+0x104` until the owner's round robin
  writes it (above), so a victim that dies before that is counted only on its owner's peer.
- **The other decoders carry no unit index.** `0x44E9C0` (the `0x490A10` selector's 2): `[1]` flags,
  `[32] × 6` and, under flag bit 0, `[16]` into `+0x24`, which the class's `0x44EA60` (vtable
  `0x4FD3F8` `+0x20`) takes as a signed angle to step toward, by the def's `+0x1BA` over 8 — a
  heading [INFERRED from `0x44EABA..0x44EB0F`], not an index. `0x44F5C0` (`0x4FD488`): one bit, a 2-bit count
  into `+0x18`, and that many `[16]`,`[16]` pairs into `+0x0C..+0x16` — three pairs of room, and the
  count cannot exceed 3. `0x490A10` itself: the 2-bit selector and a 2-bit value handed to
  `0x43D210` (`0x490A7C..0x490A8C`). `0x44EFD0` reads nothing.

**The `0x0A` receiver** is bounded at its case, not inside `0x48AB70`, whose other callers are
local ([its call sites](units.md#0x48ab70-attach-and-detach-one-unit-to-another-mapped-by-us)): B3's stub
replaces the case's `mov eax,[esp+0x10]; push eax` at `0x4553FE` (entry 8 of the jump table, read
at `0x455FA4`) and requires the child in `[1, max]` and the parent 0 or in `[1, max]`; a record
that fails goes to `0x455F50`, the pump's back edge, which every case and every code the table
does not handle (`0x45486D`'s `ja`, and entries 1, 2, 35 and 41) jumps to, and which reloads its
own state (`0x455F50` reads `[esp+0xF0]`) before re-entering the receive at `0x453D94` (counted as a
`0a` drop). A child of 0, which `0x48AB70` ignores, drops there too: no sender writes one. The
diverged `0x0D` is dropped in `wpn_rx_fired` (`0x49D280`, [the
weapon-ID receiver](weapons.md#a-weapons-id-and-the-messages-that-carry-it-0x42e440-0x49d270-0x45544d-0x42454b-disassembled-measured-2026-09-24)) by resolving the shooter's own slot weapon and comparing it to
`&Weapons[id]`: stock divides by the local slot weapon's `+0x68` (`0x49CE62..0x49CE6A`) and faults only
when that is 0, otherwise building a projectile from the packet weapon's branch and the local
weapon's velocity. Between consistent peers the two never differ (DISASSEMBLED): the sender's aim
routine `0x49D580` fires the weapon of the shooter's own slot, `[slot+0xC]` read at `0x49D742` (its
flags `+0x111`, `0x49D74B`, choose `0x49CDE0` at `0x49D76E` or `0x49C9C0` at `0x49D77E`), and when
`[main+0x2A44]` bit 0 is set (`0x49D7A0` [INFERRED: a network game]; the pump tests the same bit at
`0x453D4B`) builds the `0x0D` from that same slot — the weapon's ID byte `+0x10A` into `+0x19`
(`0x49D7D6..0x49D7F5`), the slot's number (bits 2–3 of its `+0x1B`) into `+0x23` (`0x49D7ED`) — and
broadcasts it through `0x451DF0` (`0x49D859`).

### Unit identity on the wire: a hit names its victim by slot, and first-free reuses a slot at once — `0x0B`, `0x485F50` [DISASSEMBLED + MEASURED 2026-09-25]

Landing B4 of [the port's section B](../tadr-port/sim-fixes.md) ("B4 DESIGN"). Nothing on the wire
says which unit a slot held when a message was made, and the owner hands a freed slot to its next
create in the same tick, so a hit in flight lands on whatever the slot holds when it arrives.

**The hit, `0x0B`, 9 bytes.** Built by `0x489BB0` (twelve callers): `[0]` `0x0B`, `[1]` `u16` the
victim's index, `[3]` `u16` the attacker's (0 for none), `[5]` `u16` the damage (after armour, except
for kind `0x0A`), `[7]` a byte argument, `[8]` the kind (`0x0A` heal, `0x0B` never sent, `2`
paralysis: its path pushes `0x508D80`, "paralyze", at `0x489E49`). It applies the hit locally first (`0x489C89`),
then sends it only when the victim's player (`+0x96`) exists with type 3 at `+0x73`, a remote
player (`0x489C99`), and the kind is not `0x0B`: from the attacker's player's DPID (`+0x96` → `+4`,
`0x489CB9`), or the local player's (`0x44FDB0`, `0x489CCD`) when there is no attacker; both sends
take the length 9 from one `push 0x9` at `0x489CA8`, before the attacker test. **The
receiver `0x489CE0(rec)`** refuses a victim that is not alive (`+0x110` bit `0x10000000`) or already
pending death (bit `0x4000`) (`0x489D45`, `0x489D50`); a heal adds to hit points up to the type's
maximum (`0x489D59..0x489D80`); any other kind calls `0x467950(victim)` and, but for kind `0x0B`,
`0x406F80(attacker, victim, damage)`, and records the kind at `+0xF5` and the attacker at `+0xF0`/
`+0xF4`. Kind 2 then leaves by `0x489DF5` without touching hit points, and paralyses only a
victim whose player is local (`0x489E24`); the rest subtract from them (`0x489EB5`), and hit points
at or below 0 set pending death only when the victim's player is local (`+0x73` 1 or 2,
`0x489EC6..0x489EE5`), a remote copy's being clamped to 0 (`0x489EF1`). `0x406F80` tells the
victim's orders (`0x4897B0`, event `0x10`) and does everything else only for a local victim
(`0x406FC3`, `0x407013`). So every peer applies every hit to its own copy and the owner alone decides
the death. The round robin carries hit points (`0x48B235`, written at `0x48B4B2`), so a bystander's
copy is rewritten within N ticks.

**The create, `0x09`, 23 bytes.** Built by `0x456050(unit)` `ret 4`, whose only caller is the create
at `0x486115`: `[0]` `0x09`, `[1]` `u16` type (`+0xA6`), `[3]` `u16` index (`+0xA8`), `[5]` twelve
bytes from `+0x6A` (the position), `[17]` `u32` from `+0x64`, `[21]` `u16` from `+0x68` [INFERRED:
the orientation]; its send `0x4560AE` takes the length 23 from `push 0x17` at `0x45605C`. Of
`0x451DF0`'s 57 callers, `0x4560AE` is the only one that sends a `0x09` and
`0x489CB9`/`0x489CCD` the only ones that send a `0x0B` (an E8 scan, and the type byte each call site
stores or builds). Every byte of both messages carries a field; neither has room for more.

**The allocator.** `0x485F50` (`ret 0x20`, eleven callers) walks the player's block
(`[player+0x67]`..`[player+0x6B]`, read as `main+0x1BCA`/`+0x1BCE` + `k·0x14B`) and takes the first
slot whose type `+0xA6` is 0 (`0x486036`, `cmp word [esi+0xA6],0; je 0x48605D`). A free slot goes on
at `0x48605D` (`test esi,esi`), the take, which writes the type at `0x486086`; a taken one goes to
`0x486040`, the step: `test dx,dx` ends a requested index there (`jne 0x4861BD`, which returns NULL
like `0x486053`), and otherwise the next slot is tried while it is at most `[player+0x6B]`
(`0x486049..0x486051`). Its arg 8 is a requested index: then only that slot is
tried, and only when it lies inside the owner's block `[+0x67, +0x6B]` (`0x486026..0x486030`, NULL
otherwise). All eleven callers pass 0 but the saved-game restore `0x48718E`, which passes the saved
index. It returns NULL when no slot is free (`0x486053`), and below the cap for a type 0
(`0x485F7C`), a type without def `+0x241` bit `0x800000` (`0x485FAA`) and a type at its own
limit, def `+0x15A` (`0x485FE4`). **Its callers and NULL**: the four factory and build callers
(`0x4028EA`, `0x403D5B`, `0x405104`, `0x41409B`) link the result to the order (`0x489690` takes
NULL) and test the link: NULL says "Unable to create any more units" (`0x4028FF` → `0x47F780`),
and the first three sleep the order 300 ticks (`0x439E80`) while `0x41409B` returns 8 at once
(`0x4140BD`); `0x41794F` ignores it; `0x48718E`, `0x488462` and `0x488700` test it; `0x497002`
and `0x4977BB` are the level load's; **`0x4653D9`**, the Deathmatch respawn below, uses it at
`0x465414` with no test, so stock faults there on every NULL.

**The Deathmatch respawn, `0x4653D9`** [DISASSEMBLED; its trigger's meaning INFERRED]. It is in
the players phase `0x464F80`, which walks the ten players and does a player's periodic work only
on its pass, when GameTime reaches its `+0xF0` (`0x465083`, then `+0xF0 += 0x1E`: every 30
ticks). On the controlled player's pass (`main+0x2A42`, `0x46509D`) outside a campaign
(`0x435100` ≠ 1), with the options word's bit `0x40` clear (`[player+0x27]+0x9B`, `0x465154`) and
`0x490360` nonzero (a test on `main+0x391ED` with a GameTime-keyed timer at `0x51E6C4`), it runs
the countdown **`main+0x39239`**: below 0 it reloads to 4 and stops (`0x46518B`), else it
decrements, and on reaching −1 it fires (`0x4651A4`). A fire with `ActiveCommanderDeath`
(`main+0x37EF6`) at 2 creates a unit of a type named in the side's record (`0x488B10`)
[INFERRED: the side's commander] for the controlled player at a free place: up to 9999 random
points (`0x465236`), each tested over a 3 × 3 block of cells (`0x47DB70`); at any other value it
goes to `0x465643`. That acts only in a network game (`0x435100` = 3, `0x465643..0x465651`), for
a record whose `+0x22` is 0, and when the seat `0x456850` names has PlayerInfo `+0x9B` bit `0x80`
or `0x457BC0` answers above 0 (`0x465657..0x465694`); it sets the player's PlayerInfo `+0x9B` bit
`0x40` (`0x46569D`, the bit `0x465154` tests, so the path stops; the radar pass shows a viewer with
it every unit, `0x4674E4`). For the controlled player it then clears LosType bits 0 and 1
(`0x4656B5`, `0x4656C5`), calls `0x4816A0(1)` (`0x4656CE`) and `0x450F90`, and shows "You're out!
Continue Watching?" (`0x507318`, pushed at `0x465763`) when `0x457BC0` answers 0, else the
hosting-AI watch notice (`0x507338`, `0x4657FE`) when `0x457CB0` answers above 0. A record that
fails any of the three tests goes to the game-over bits of `main+0x3923B` (`0x46582E`). Mode 2 is
"Deathmatch" (the in-game options label, `0x45F279`), reachable from the
multiplayer battle room, whose button cycles 0 → 1 → 2 → 0 (`0x448504..0x448546`, the options
word's bits 11–12), and not from the skirmish screen, whose toggle is `xor [options+0x108],1`
(`0x47B690`). **The countdown's readers and writers**: set to `0xFFFF` at a level's entry
(`0x498199`); the respawn's path above (`0x46517F..0x4651A4`); two objective copies that set
bits of `main+0x3923B` at their fire, `0x4650F3..0x465118` (a campaign, behind `0x490360`) and
`0x465881..0x4658A6` (behind `0x490230`, campaign or not); a copy after the loop
(`0x4655E8..0x465610`, a network game whose `ActiveCommanderDeath` is not 2, behind `0x457CB0`)
[INFERRED: the end of a network game]; and **`0x46554F`**, on every
player's 30-tick pass, before the economy pass `0x401360(player)` at `0x46555A`, its only call (no
stored pointer). The call is made for a record with a nonzero first dword, `+0x73` 1–3 and `+0x146`
≠ 10 (`0x465508..0x465523`), not for one with no live units that has created some (`+0x144` = 0,
`+0x140` ≠ 0; `0x465525..0x465537`), for `+0x73` 1 or 2 only (`0x465539..0x46553F`), while
`main+0x3923B` bit 2 is clear (`0x465546`) and while the countdown is below 0 (`0x46554F`).
`0x401360` zeroes the storage maxima `+0xA4`/`+0xA8` (`0x40137D`, `0x401383`) and re-sums them
from each live, finished unit's (`+0x104` = 0.0, `0x4016C5..0x4016D6`) def `+0x1E2` (energy) and
`+0x1E6` (metal) (`0x4017A1..0x4017C5`), folds in
the `+0xEC` record (`0x40190E..0x40193D`), adds the base storage `+0xDC`/`+0xE0` only while
`+0x149` bit 0 is set (`0x401941`, `0x401988..0x4019AA`), writes production and expense
`+0x90`/`+0x94`/`+0x9C`/`+0xA0` and adds them to the totals `+0xAC`/`+0xBC`/`+0xB4`/`+0xC4`
(`0x4019C6..0x401A3D`), adds production to current (`0x401A1D`, `0x401A25`), subtracts the
consumption up to what there is, keeping the fraction met per resource (`0x401A4D..0x401AB1`), and
clamps `+0x8C`/`+0x98` to storage, the excess into wasted `+0xCC`/`+0xD4`
(`0x401AB3..0x401B27`). A type-2 player's income is scaled there by the difficulty
`main+0x37EEE` (behind `0x401435`, eight reads `0x401441..0x40175E`). The type is written at `0x486086` and the `0x09` sent at `0x486115`. **The blocks go to the
records in DPID order in a network game**: `0x485842` sorts the ten record pointers on `+4` when
`0x435100` answers 3 (otherwise on the pointer itself), and `0x4858BD` gives the k-th of the sorted
list the block `1 + k·N` (`+0x67`, and its last slot at `+0x6B`, `0x4858E0`), so record k owns
block k only on the peer first in DPID order. **The type word of a
unit slot has three writers**: this create, `CreateFromNetwork` (`0x4862A2`), and the destructor `0x4866D0`'s
free (`0x486DC7`, type 0). `0x421FE2` writes the same offset in a record the feature code allocates
for itself (`0x421F9B`), and `0x485ED2` is in `0x485E90`, which nothing calls and no dword in the
image points at — dead code. **`0x4854A0`** allocates and zeroes the unit array (`0x485515`,
`0x485557`); its one call is `0x4918D4`, in the level's init. Before it allocates, it sets the slot
count, `u16 main+0x14351`, to `10·[main+0x37EE6] + 1` in 16-bit arithmetic (`0x4854E3..0x4854EF`:
`mov dx,[eax+0x37EE6]; imul dx,dx,0xA; inc edx; mov [eax+0x14351],dx`).

**`CreateFromNetwork 0x4861D0`'s exits.** It refuses at `0x48622B` (the player has no block) and
returns the unit from `0x48634F` (`mov eax,esi; pop edi; pop esi; pop ebp`, then `0x486354` `pop ebx`
… `ret 8`). At `0x48634F` its frame is the one `0x4861ED` reads the record argument from (`mov
edi,[esp+0x24]`), with the caller's return address at `[esp+0x1C]`. Its three callers are the `0x09` case (`0x4553E4`, returning to
`0x4553E9`), the `0x2C` dirty entry (`0x48BA00` → `0x48BA05`) and round robin (`0x48B497` →
`0x48B49C`). The two `0x2C` creates pass the slot's own `+0xFF` as the player (`0x48B9E7`,
`0x48B47E`), not the sender. **The `0x2C` header's GameTime** (`[32]`, read at `0x48B955`) is
stored at the sender's `player+0x18` (`0x48B963`) before the first entry.

**Replica creation observation [DISASSEMBLED 2026-10-05].** The record read at
`0x4861ED` supplies slot at `+3` (`0x4861F7`), type at `+1` (`0x48625F`) and
three position dwords at `+5` (`0x48624F..0x486263`). `0x4862B8` calls unit init
`0x485A40` with finished zero; the constructor then creates COB (`0x4862BE` →
`0x485D40`), weapon queries (`0x4862C4` → `0x49E070`), orders (`0x4862CA` →
`0x437840`), the mover when applicable, sight (`0x486306` → `0x48A870`), the
footprint (`0x48630C` → `0x47CC30`) and the bucket (`0x486312` → `0x482AC0`).
It increments the owning player's word/dword counts at `0x486322`/`0x486338`,
then calls `0x490580` at `0x48634A` before the success exit.
B4's existing exit callout applies valid carried build/HP state through
`kill_apply_state`, stores its incarnation/local clock, then calls
`tagpu_rec_network_created`. That observer copies the constructor arguments,
bounded owner, slot water and the post-B4 `+0x104`/`+0x108` values into native
`RK_EV_NET_CREATE`; it adds no byte site or engine field write. The local create
observer at `0x4861B3` does not see this constructor. Native playback remains
unfinished; the root is stored for its eventual materializer.

**Native constructor inputs and carried state [DISASSEMBLED, SOURCE-CHECKED
2026-10-05].** `0x4861D8` masks the owner to a byte, then scales it by
`0x14B`; it does not bound it to ten records. The nonzero slot from `record+3`
is scaled by `0x118` at `0x486208..0x48621D`, without a slot-count check.
The type from `record+1` indexes `main+0x1439B` with stride `0x249` at
`0x48626F..0x48627F`, without a definition-count check. The player block test
at `0x486220..0x486227` checks only a nonzero first-slot pointer. An occupied
slot's nonzero type at `0x486237` calls `Send_UnitDeath(unit,0)` at `0x486244`
before initialization. A replay caller therefore has to establish the owner,
slot, type and free-slot invariants before entering this root; the engine does
not supply those bounds, and a puppet gate refusing the death does not free it.

The mover branch tests definition `+0x22F == 1` at `0x4862CF`; it allocates
`0x2F` bytes through `0x4B4F10` at `0x4862DA`, constructs through
`0x43DC00(unit)` at `0x4862E9`, stores `unit+0`, and copies definition `+0x210`
to unit `+0x66` at `0x4862FA..0x486301`. In B4's `hit_created`, only the
recognized carried-create return applies `kill_apply_state` before publishing
the exact birth and invoking the recorder. Its carried record uses birth at
`+23`, build fraction at `+27`, HP at `+31` and presence at `+33`. That helper
accepts only presence 1 and fraction 0..1, clears model-object `+0x10`, writes
unit `+0x108`, and, on a changed fraction, writes `+0x104` and sets state bit 13.
It calls no build-completion function. Calling the local constructor with
finished nonzero is therefore a different path from this unfinished native
constructor followed by carried state. No native playback root is enabled here.

**The dispatcher's switch and gate.** The jump table `0x455F84` is indexed by `code − 2`
(`0x454861`): `0x05` at `0x455F90` (→ `0x45522E`), `0x09` at `0x455FA0` (→ `0x4553DA`), `0x0B` at
`0x455FA8` (→ `0x45540D`), `0x0F` at `0x455FB8`. Before it, `0x45473F` gates on the state
`main+0x391F1`: `[0x512BC0 + 4·code]` bit 2 in state 5, bit 4 in state 6, bit 1 otherwise; `0x451FD0`
fills it, `0x05` with 7 (every state), `0x09` and `0x0B` with 4 (state 6 only). The subpacket lengths
are at `0x512AD8 + 4·code` (`0x05` `0x41`, `0x09` `0x17`, `0x0B` 9). A case finds the message at
`[esp+0x10]`, the player at `[esp+0x14]` and the sender's record in `edi`, whose `+0x10` counts its
messages and `+0x1C` holds the time of the last (`0x45484A..0x45485C`). Stock's chat `0x45522E`
passes the text from `msg+1` to `0x463CA0`, which returns at once when it starts with a zero byte
(`0x463CA7`).

**GameTime `main+0x38A47` has five writers** (every store to that offset): the tick's increment
`0x4954C0`, the shell's network clock `0x44A696` (in `0x44A680`, called from the menu's state
machine at `0x42837F`), and three zeroings at a level's entry — `0x491979` (just after `0x4854A0`
in the level init), `0x4971BB` (the loader), `0x498180` (the load state's first call). In play it only
increments. It is not 0 when play starts: the in-play entry runs up to five catch-up ticks in state 5
before its state-6 store (below, *A create refused during the load*).

**The send layer** (read, not patched). `0x451DF0(net, msg, len)` `ret 0xC` checks the sender
(`0x44FFD0`, type 1 or 2) and, with `[0x506DBC]` set, queues through `0x461990` → `0x462710`
(`ecx = 0x513000`): per destination, a packet of at most `0x42A` bytes and a ring of `0x400` sent
packets [INFERRED: a resend queue]. Otherwise `0x4C97B0` calls `IDirectPlay::Send` (vtable `+0x68`)
to DPID 0, every player, guaranteed when `[0x50A780]` is set. **Whether one peer's messages reach
another in the order sent is not established** (the port's open question); B4 does not rely on it.

**What B4 patches** (the design and its argument are in the plan; the code is `fix_stale_hits`):
the three sends above call ours with the send's own signature and put the stock record inside a
tagged `0x05` (`05 00 4A` + the `0x09` + the owner's birth, `05 00 4B` + the `0x0B` + the sender's
stamp of its victim), and the stock lengths the tagged copy takes, `push 0x17` at `0x45605C` and `push
0x9` at `0x489CA8`, are compared rows of the table, not written; the `0x05` slot `0x455F90` becomes
the receiver, which applies the carried
type's own gate and enters stock's handler with the return address stock's case pushes (`0x4553E9`,
`0x455417`), so B3's bounds and its counters, which key on them, run unchanged; the `0x09` and `0x0B`
slots drop a bare message; `0x48634F` stamps a copy (exact after a carried `0x09`, its birth
read from `CreateFromNetwork`'s own record argument; after a recreate, the `0x2C`'s GameTime as a
lower bound, from the record whose block holds the slot), then replays `mov eax,esi; pop edi; pop
esi; pop ebp` and goes on at `0x486354`; `0x486036` decides first-free once, at
the block's first free slot: an unheld one, else the one freed longest ago if in an earlier tick,
never one freed this tick (arg 8 = 0 only) — a slot it takes goes on at `0x48605D`, a taken one to
`0x486040`, and with none to take it leaves `esi` at the block's last slot and goes to `0x486040`,
whose step ends the loop at `0x486053`, NULL, as at the unit cap; `0x486DC1` stamps the free;
`0x4854A0` resets the tables with the array and sizes them from its count, computed as
`0x4854E3..0x4854EF` computes it, in place of `sub esp,0x30; mov eax,[0x511DE8]`, going on at
`0x4854A8`; `0x4653DE` sends a NULL from `0x4653D9` to the block's end `0x4654FB` (a unit goes on at
`0x4653E6`, after the replayed `mov esi,eax; xor eax,eax; mov edi,[esp+0x34]`). `0x4654FB` runs on
the site's own stack — the calls between clean their own arguments: `0x496E90` (`ret 0xC` at
`0x496ED4`; it sets bit 0 of its first argument's `+0x149` and stores the other two, each at least
200, as floats at `+0xE0` and `+0xDC`), `Game_SetLOSState 0x4816A0` and `0x48D630` (each `ret 4`,
pushed `1` at `0x4654ED` and `0x4654F4`) — and reloads `edi`, `bl` and `ebp` itself
(`0x4654FB..0x465503`). The stub writes nothing, so the countdown stays at −1 as stock's fire leaves
it and fires again six passes later while its trigger holds. All are rows of
the fail-closed table, in both builds.
MEASURED 2026-09-25 (the plan's *B4 LANDED* section, three peers, every hit held 30 ticks by the test
lever): hits applied to a unit younger than the delay fell from 1 562 to 0 on the owner and from
1 406 to 1 on a bystander (0 in a second run); a hit costs 65 bytes on the wire instead of 9. After
the fix round the owner still applied none young, both joiners computed real lower bounds with
none unknown, and a bystander applied 52 undecidable hits on a `0x2C`-made commander copy.

### The unit sync's keys, and the join's pace — `0x4B6BA0`, def `+0x13E`, `0x46D6C0..0x46DEC8` [DISASSEMBLED + MEASURED 2026-09-24]

**The key.** The menu-time loader `0x42A8D0` reads each FBI whole (`0x4BB7C0`) and stores
`0x4B6BA0(bytes, size)` at def `+0x13E` (`0x42ABB3`, `0x42ABBC`). `0x4B6BA0` is four 8-bit lanes, not a
CRC: with `i` the offset's low byte and `b` the byte, the key is `xor(i+b) << 24 | sum(i^b) << 16 |
xor(b) << 8 | sum(b)`. A `units\<name>.OVR` file with a `TA Unit Override` header can replace it with
its `Compatability` value (`0x42ABCB..0x42AC43`; the file's shape is [INFERRED] from the strings
`units`, `OVR`, `TA Unit Override`, `Compatability`, `%u`). The def clone `0x42B370` copies it. The
**value** at `+0x142` is filled lazily by `0x42A610`, only while it is 0: the xor of `0x4B6BA0` over
`scripts\<name>.cob`, every `guis\<name>*.gui` (`0x4BCA30` enumerates them) and
`download\<name>.tdf`, and then of `+0x146` [role INFERRED]. It reads files, so its first call for a
type costs a file lookup and a read.

**The engine write-protects the def array.** `0x4D86B0(ptr, size, prot)` calls `VirtualProtect` over
the range's whole pages when `0x4D8140` answers nonzero (`[0x5289A0]`, set once through `0x4D9FE0`
with the `gonzo` switch at `0x50C290`/`0x50C298`); `0x4D8690` and `0x4D86F0` pass `PAGE_READONLY` and
`PAGE_READWRITE` for a range, and `0x4D8710` and `0x4D8780` do the same for a whole heap block
(`0x4D8720`, its size from `0x4D8360`). The loader seals the def array at its success exit
(`0x42B328`), and a writer opens and seals it around its writes (`0x42A610`: `0x42A63A`, `0x42A829`).
MEASURED: protection is on in a plain launch; a write into the array after the load faulted.

**The loader's call.** `0x42A8D0` has one call, `0x42BD29` in `0x42BD10`, which runs it when
`UNITINFOCount` is 0 or the reload flag `[main+0x14397]` is set and then clears the flag;
`0x42BD10`'s one call is the state callback `0x496BB0`. The loader has two exits: `0x42B357` returns 1
after the seal, and `0x42B200` returns 0 without it, when a unit file has no `[UNITINFO]` section
(`0x42AC78`).

**The shell and a level hold the unit types in different orders.** MEASURED 2026-10-02 on retail,
reading `UNITINFOCount` and every UnitName (`+0x20`) through `tacli peek`:
- At the main menu, before the first level and again after one, the count was 279 and the
  names stood in the start-up load's order (`None`, `ARMSEER`, `ARMSILO`, …). Their FNV-1a, as
  `RT_UNITDEFS` takes it, was `0x93B2B33D`.
- In a level, the same 279 names stood sorted, `None` first (`ARMAAP`, `ARMAAS`, `ARMACA`, …),
  hash `0x54F5FE19`, which is what the level's recording carried.
So a type ID, and the defs fingerprint, mean what they mean in a level only once the level has
loaded. The level's unit-data load `0x42D2E0` makes that order and then re-reads every name from
its file; the shell's order comes back because that load ends by setting the reload flag
(`0x42DB2B`), so the next `0x42BD10` runs the loader again (*`0x42D2E0` — the level's unit-data
load*, below, has the steps). That is why Watch compares the defs at the first step
(`tagpu_replay.c`, `data_match`), and why the Replays browser locks a recording only once Watch
has refused it (the S5 note, *The lock*).

**The sync.** The object is `[main+0x2A30]`: `+0x58` nonzero on the host, `+0x64` a disable flag, the
per-peer records of 0x5C bytes at `[+0x14, +0x18)`, a tree of keys at `+4` (`0x46E9B0` finds,
`0x46EF50` inserts), a circular list of keys at `+0x24` with its size at `+0x28`, and the joiner's
cursor into the def array at `+0x60`. A peer's record holds the keys received from it (a vector,
`[+8, +0xC)`), their values (`[+0x18, +0x1C)`), the count it announced (`+0x24`), and `+0x28` and
`+0x2C`, which the status prints as `packets sent=%d  ackd=%d`. Its message is type `0x1A`, 14 bytes: the subtype at `+1`, a
sequence at `+2`, the key at `+6`, a value or count at `+0xA`.

- **The joiner's tick** (`0x46DD52`, in `0x46DAD0`, called from `0x44AF44`; about 220 times a second
  in the measured lobby) sends subtype 1 with its number of types, then subtype 2 — a key and its value from `0x42A610` (`0x46DE28`) — for
  **four types a tick** (`0x46DDFA..0x46DE95`, `cmp ebp,4` at `0x46DE8F`), and subtype 4 every tick
  once the cursor passes the count (`0x46DEA3`).
- **The host's receive** `0x46D6C0` finds the sender's record (linear over the peers) and switches on
  the subtype (table `0x46D84C`): 1 stores the count (`+0x24`), 4 keeps the largest count (`+0x2C`),
  and 2 (`0x46D748`), when the record has not seen the key (`0x46D755`, linear), appends the key and
  value to the record's two vectors (`0x46E640`) and calls `0x46D970`. That finds the key in the
  tree, or walks the defs for the first with the key (`0x46D9E3`) and fills its value, compares it
  with the value each peer sent for the key (`0x46DA7A`), and calls `0x46D860`, which sends subtype 3 for the key to every
  peer (`0x46D4C0`) and appends the key to the list unless the list holds it (`0x46D906`, linear).
- **The joiner's receive** takes subtype 3 only (`0x46D7C0`): the key into its tree, then
  `0x46D860`.
- **The host's tick** (`0x46DAE4..0x46DD4A`) drops the records of players gone, makes records for
  new ones, and when either happened runs `0x46D970` for every key in the tree (`0x46DD1E`).
- **The room waits** until the peers' records are complete (`0x46E000`): the keys received number
  the count announced, which must not be 0, and `+0x28` equals `+0x2C`. It skips a peer
  `0x44FED0` does not find and one whose state byte `+0x73` is 2, or 3 with `+0x94` at 2. The
  `+syncerr` chat command (the text compared at `0x448427`, `0x46DF40` called at `0x44843F`) says
  which is missing: `No units_expected sent from player`, `expected %d units, got %d`, or the
  packet counts.
- The sequenced channel (`+0x2C`, receive `0x46CEF0`) answers subtype `0x65` with a resend from its
  history (`0x46CF12`, linear); `0x46D500` is the unsequenced path into `0x46D6C0`.
- **Other readers of the key.** The restriction screen's saved lists, `*.LST`, hold a key and a value
  a type: `0x44B230` writes them and `0x44B140` reads them back, applying each to the first def
  with the key (a walk from `defs + 0x387`, def 1's `+0x13E`, so a search for `0x13E` misses it).
  `0x44C2B7`, the restriction pictures, reads it too. No save-game or in-game packet does.

**What goes wrong.** A joining peer with two types of one key sends that key twice, the host keeps it
once (`0x46D755`), and its record for the peer never reaches the count the peer announced: the
battle room shows SYNCHING for good. On the host, a shared key answers with the first type that
has it (`0x46D9E3`). MEASURED: 16 105 generated FBIs that differ only in their digits gave 9 991
keys, the host's list of keys stopped at 10 269 of 16 383 (the checksum above, computed over the
install's FBIs and the generated ones, gives the same count), and the join had not ended after 10
minutes. The time a join
takes is the joiner's pace, not the searches: at 16 383 types with unique keys, 20 s, the cursor at a
steady ~880 types a second; at 64 a tick, 12.5 s; and a rejoin in the same processes, whose values
`0x42A610` had already filled, under 2 s, while the host's record for the joiner was new and its
searches ran again. So what a first join at the ceiling still costs is the value's file reads.
Stock content joins in under 2 s.

**The def array's order is the order the files were found in.** The loader's list comes from
`0x4BCA30`: loose files first (the C runtime's find-first, `0x4E7DD0`), then the archives, which
`0x41D4C0` opens as `rev31.GP3`, `*.CCX`, `*.UFO` and `*.HPI`, each pattern in the order the
directory lists them (`0x4BC4B0`). Nothing sorts it before the game load (`0x42D533..0x42D610`, by name through
`0x42DB60`); the menu-time load's end only fills the slot of a type it drops with the last def
(`0x42B2A3..0x42B2BB`, through `0x42B370`). So two peers with the same files can hold the types in a different order: a
different file system, a renamed archive, a loose copy of a file.

**The fixes.** `fix_sync_keys` hooks the loader's call (`0x42BD29`): when it returns 1, the types are
taken by key and then by name (`+0x20`, `strncmp` over its 32 bytes); in each group that shares a
key the first keeps it, and each other type takes an FNV-1a hash of its name, moved on past every
value a type holds or has been given. So every key is a function of the loaded types' names and
natural keys, not of their order. The array is opened with `0x4D8780` for the writes and sealed again
with `0x4D8710`. Types that do not collide keep their keys, so stock content is unchanged: the
install's 821 FBI files, 278 names, share no key between two names. The pace, a raised-limits site:
64 types a tick (`0x46DE91`). MEASURED: the colliding 16 105 re-keyed 6 114 types identically on
both peers, and the join ended in 13 s. And 2000 colliding types split over two archives, every
other type in each, the archive names swapped on the second peer so that every type sat at another
index: a re-key by index gave 357 types a different key on each peer, and those 357 vanished from
both at the start with no message; this one gives all 2000 the same key on both, and all remain.

What the fix leaves:

- **A peer without it** (stock TA, or a build before it) in a game with a mod whose keys collide:
  as a joiner it stalls the room as stock does; as the host it holds no entry for the keys the
  others were given, so `0x46D970` gives those types no verdict, and the types' availability can
  differ between the peers. Every peer runs the same build (the raised limits' contract).
- **A saved `*.LST` list** names a re-keyed type by its given key. That key is the first free value
  from a hash of the name, so it moves when the types around it change: a type that held a value
  in its run comes or goes, or a type with the same natural key and an earlier name arrives and
  takes the natural key, re-keying the old keeper. The list's entry then applies to whichever type
  holds the key. A list saved by stock gives a re-keyed type nothing, and in stock a shared key
  applied to the first type with it.
- **Two types with one name and one key** cannot be told apart by the sync either; which of them
  keeps the key follows the def array's order.
