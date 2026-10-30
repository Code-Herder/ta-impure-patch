# The demo recorder's engine boundaries

Where the recorder captures, the event sites, the puppet host, the takeover and the replay's engine-facing state.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

## Demo recorder exploration: capture and save boundaries [DISASSEMBLED 2026-09-28]

Boundary recheck for [the recorder exploration](../tadr-port/demo-recorder-exploration.md).
Temporary observers measured the three DirectPlay wrappers below in a two-peer game; they
were removed and the normal DLL rebuilt. The multiplayer contribution work branch uses a separate native application/session; the receive wrapper remains stock. That checkpoint has not landed. The
existing send/receive and save investigations below remain the fuller maps.

| Address | Fact checked and implication |
|---|---|
| `0x451DF0` | Message-send entry, `ret 0xC`; reads the main pointer `0x511DE8`, sender/player records, network bit `main+0x2A44`, and branch selector `main+0x299C`. With that selector zero, `[0x506DBC] != 0` calls `0x461990` at `0x451EE0`, `ecx = 0x513000`; otherwise `0x451EFD` calls `0x4C97B0` with destination 0 and context `main+0x14`. The other branch loops player records at stride `0x14B`, using deduplication storage `0x512B90`, and calls `0x451BC0` at `0x451F75`. This entry is already B8's held-send gate, not an unowned recorder hook site. |
| `0x4C97B0` | `ret 0x14`; after the existing diagnostic call `0x4C9740`, takes the interface at context `+0x4C5`, calls its vtable `+0x68` (`Send`) at `0x4C97F6`, and derives its flags from `0x50A780`. A NULL interface returns `0x887700AA`. The adjacent `0x4C9800` wrapper also calls `+0x68` (at `0x4C9839`), with flag 1; recording only one wrapper would need a coverage argument. |
| `0x4C9840` | `ret 0xC`; reads the same interface at context `+0x4C5`, passes context `+0x4B5` and `+0x4B9` as the sender/destination output addresses and flag 1, then calls vtable `+0x64` (`Receive`) at `0x4C987E`. The buffer and length pointer come from its other two arguments. These offsets are relative to the network context, not the main block. |
| `0x4534E0`, `0x453595`, `0x45361F` | Existing bounded-delivery capture in B3, re-read in `tagpu_patches.c:wire_rx_note`: the latter two sites replace the statistics call and preserve the delivered buffer/length in TLS. That moved-buffer fix remains the fallback when the owned primary installer refuses. See *One receive, one message* below for callers, fields, and prior disassembly; not a newly verified whole-function trace in this pass. |
| `0x432A38` | TADR `SaveGame_SaveAdditionHook`'s site: retail bytes `A1 E8 1D 51 00`, `mov eax,[0x511DE8]`. Continuation `0x432A3D` takes the stack-local address, reads `main+0x391ED`, and calls `0x48FDF0` at `0x432A48`. The field's object type and the called function's meaning are not established by this slice. |
| `0x43267D` | TADR `SaveGame_LoadAdditionHook`'s site: retail bytes `8B 15 E8 1D 51 00`, `mov edx,[0x511DE8]`; continuation `0x432683` pushes `esi` and reads the same `main+0x391ED`. Matching splice bytes establish neither checkpoint completeness nor multiplayer restore support. |

Reproduce the disassembly with `i686-w64-mingw32-objdump -d -Mintel
--start-address=<start> --stop-address=<end> pristine/TotalA.exe.pristine` in the main checkout:
`0x451DF0..0x451FD0`, `0x4C97B0..0x4C9890`, `0x432A38..0x432A4D`, and
`0x43267D..0x43268A` (exclusive stops). The untraced enclosing save/load routines remain open;
their Pascal callers are source evidence, not newly assigned engine names.

The DirectPlay creation forwarders in `tagpu_takeover.c` were also read: they delegate the
existing import routes documented above (`0x4CA435`, `0x4CA4D7`, `0x4CA667`, `0x4CA922`) and
do not themselves wrap the returned COM interfaces. The render exchange was read separately:
its latest-frame contract and camera-limited contents are not an ordered event log or a
complete replay checkpoint. See the exploration for the source references and remaining gaps.

### Temporary transport observation and construction reads [MEASURED 2026-09-28]

The three observed entry instructions are position-independent five-byte `push imm32`s:
`0x4C97B0 = 68 E8 B0 50 00`, `0x4C9800 = 68 FC B0 50 00`,
`0x4C9840 = 68 1C B1 50 00`. The send wrappers' stack arguments are
`(context, sender, destination, buffer, byte_count)`; the receive wrapper's are
`(context, buffer, byte_count_pointer)`. These signatures were checked against the disassembly
above and used by the temporary observer. Receive copying happened only after return value 0
and with returned length no larger than the original caller-supplied capacity. Sender and
destination were read from that call's context `+0x4B5/+0x4B9` before returning to its caller.
The capture retained copied bytes, never an engine buffer pointer.

The measured workload was Two Continents, two native-DirectPlay peers, 900 newly spawned
artillery units plus two commanders, followed by seven synthetic unfinished units. Both
send wrappers and the receive wrapper armed. The host/joiner captures hold 1821/1795 records;
ordered `0x4A/0x4B/0x4C` payloads agree exactly between the originating and receiving peer.
This is transport evidence, not replay correctness. See the exploration for counts, codec
measurements, observer limitations and local evidence inputs.

Read through `tacli peek` at `*0x511DE8+0x38A51`: both peers' pause byte was 1; their
`+0x38A47` ticks were 3674 and 3683. On the receiving peer the unit-array pointers at
`+0x14357/+0x1435B` differed by 4,200,000 bytes (15,000 strides of `0x118`; the latter
pointer is the last slot, as the existing publisher map documents). The inspected slots
68, 69, 70, 71, 78, 79 and 80 are strictly inside that span. Their six bytes at
`unit+0x104` contain the build fraction remaining followed by the HP word at `+0x108`.
Six matched the owner's carried create state; slot 79's fraction was 0 rather than the
owner roster's 0.60, with HP 1076 still matching. A subsequent stock `0x12` with both slot
words 79 exists in both captures. That message's causal role remains an inference; the
synthetic scenario first creates a finished lab and then changes its build fraction.

The source-only inventory also re-read the publisher's existing in-play ownership boundary
(`0x468CF0`, return `0x4969D2`) and its loader handoff; no new behavior at those sites was
measured. Pascal's `TPlayerStruct` fields `LOS_MEMORY +0x7C`, dimensions `+0x80/+0x84`,
length `+0x88`, and resources `+0x8C` are candidate leads from `TAMem/TA_MemoryStructures.pas`,
not a new disassembly or live verification of remote-player visibility/statistics.

### Replay asset identities and procedural frames [DISASSEMBLED / SOURCE / MEASURED 2026-09-28]

The [portable scene experiment](../tadr-port/demo-recorder-exploration.md#6e-portable-world-assets-and-generated-explosion-frames)
resolves loaded content without requiring a live representative of each recorded unit type.
These are temporary owner-thread inventory reads, not new production hooks or a general
validator for hostile game assets.

**Animation-bank cache, DIS:** `0x429700` reads count `main+0x147AB` at `0x42970B` and
base `main+0x147AF` at `0x429723`. Each `0x44`-byte record contains a name followed by the
bank pointer at `+0x40`; search calls `0x4F8A70` at `0x42972D`, advances by `0x44` at
`0x429743`, and checks the engine count at `0x429746`. A miss builds the path through
`0x4290F0` at `0x42975E`, loads through `0x4B8C60` at `0x429768`, reallocates
`(count+1)*0x44` bytes through `0x4D84A0` at `0x4297A1`, and publishes the new base at
`0x4297AF`. It copies the name at `0x4297D1..0x4297EC`, stores the bank at `0x429807`,
and increments count **after initialization**, at `0x429817`. Cache hits return `+0x40`
at `0x429840`. This ordering does not authorize unsynchronized reads during reallocation.
`0x429850` forwards sequence lookup to `0x4B8D40` at `0x42985A`. The effects initializer
`0x429870` calls this bank accessor at `0x42987E` and stores the bank at `main+0x148F3`
(`0x42988E`); its named sequences occupy individual fields, not the entire surrounding range.

**Loaded GAF layout, DIS:** `0x4B8C60` calls file loader `0x4BBE50` at `0x4B8C6B`,
null-checks the result, reads a signed 16-bit sequence count at bank `+4` (`0x4B8C7D`),
and walks the pointer table at `+0x0C` with stride 4 (`0x4B8C91`). The fixup at
`0x4B8CA8` converts a sequence offset into a pointer. Each sequence has a 16-bit frame
count at `+0`, name at `+8`, and frame entries at `+0x28`, stride 8. Named lookup
`0x4B8D40` compares the requested name to sequence `+8` via `0x4F8A70` at
`0x4B8D5E..0x4B8D62`, returns the matching pointer at `0x4B8D83`, and returns null
for an empty bank or no match (`0x4B8D7A/0x4B8D8F`).
Frame pointers are fixed at `0x4B8CBA`, pixel pointers at frame `+0x10` at `0x4B8CC7`,
and subframe pointers/pixels at `0x4B8CD2..0x4B8CF5`, using the byte count at frame `+0x0A`.
Nothing else in a frame header is rewritten, and the GUI blits `0x4B7F90` and `0x4B8310` read
`+0x00..+0x0A` and `+0x10` only (DISASSEMBLED 2026-10-01), so the dword at `+0x0C` reaches memory
as the file wrote it and no engine path reads it. It is 0 in all 14 589 frames of the vendored mod
corpus (MEASURED 2026-10-01); Impure's own art marks itself there (`tagpu_gaf.h`,
`TAGPU_GAF_EXACT`).
These loops use file counts; this inspection does **not** establish bounds validation against
file allocation size. The prototype indexes already-loaded matching content on its owning
thread, caps traversals, and identifies frames by bank/sequence ordinal/name/frame index.

**Unit and wreck templates, SOURCE / DIS:** `tagpu_model3do.h` supplies bounded model walks
from roots in `main+0x14377`, indexed by unit-definition index; definitions at `main+0x1439B`
have stride `0x249` and names at `+0x20`. Template node identity uses unit name and walk
ordinal/piece name. Face GAF references use the existing face discriminator and frame/sequence
resolvers. A feature definition's `+0xFE` bit 0 is a required type discriminator: when clear,
`+0x98` is the 3D template root; when set, the eight fields `+0xAC..+0xC8` are GAF sequences.
The 3D creation branch `0x423D96..0x423EDB` reads that root at `0x423EBB`, passes it to
`0x45A8D0` at `0x423EC2`, and stores the new Object3do at wreck record `+4` at `0x423ECB`.
It uses the wreck pool `main+0x1420B`, stride `0x30`, and free bookkeeping at `+0x1421B`;
records receive definition `+0x2C`, tick `+0x26`, position `+8/+0x0C/+0x10` and rotation
`+0x20/+0x24`. The tile receives definition `+8`, record index `+0x0A`, and flag bit 0
at `0x423ED9`; the GAF branch begins at `0x423EDD`. Only the 3D branch gives the spawn a
return value (the local zeroed at `0x423C60`, set at `0x423E15`, returned at `0x424038`): a GAF
feature such as a tree is placed by the anchor cell's `+8` word alone (`0x423EE5`, with `+0x0A`
zeroed at `0x423EE9`) and the spawn returns 0 for it, as for the refusals (def `0xFFFF` at
`0x423C6A`; `0xFFFC` writes the word and returns 0, `0x423C7D..0x423C85`). **Negative result:** traversing the
GAF fields of a 3D definition crashed the initial inventory. A pointer sanity check would
not repair that type error; branching on the engine's discriminator did.

**Generated explosion art is match state, DIS:** level initializer `0x420620` calls
`0x4B91B0` at `0x42063A` and stores that result at `main+0x1AB9B` (`0x420654`). Its three
sequence allocations use `0x4D83B0`; frame entries have the GAF `+0x28`, stride-8 layout.
Calls to frame generator `0x420D20` at `0x4206C7`, `0x420740` and `0x4207BC` fill sequences
stored at `main+0x1AB8F/+0x1AB93/+0x1AB97` (`0x4206E3/0x42075F/0x4207E2`). With the
retail values assigned at `0x42065F..0x4206A8` to `0x511F90..0x511FB0`, the sequences have 12, 15 and 15 frames; their
square side lengths start at 64, 128 and 200 and decrement by 4, 7 and 11 respectively.
The initializer also writes `main+0x38D74` with 20, 50 and 100; its broader meaning is not
established here. Existing level-load/teardown call sites and reclaim lifetime are mapped in
the effects section below.

`0x420D20(side)` calls GAF allocator `0x4B8DA0` at `0x420D49`, sets half-side hotspots at
frame `+4/+6` (`0x420D5B/0x420D61`), then calls the CRT random function `0x4E4870` **once
per pixel** at `0x420D99`. Integer helpers `0x4E4400/0x4E4440` scale that output by
10/32768. The pixel value combines this noise with an elliptical radial distance, using
double constants at `0x4FD010/+8` and integer conversion helper `0x4E43A0`; their numerical
values were not needed for the copy experiment. Output palette bytes at `0x420DED/0x420DFD/
0x420E0B` are `0xFF`, `0x6E`, or the calculated value plus `0x4F`. The loop writes through
frame `+0x10`, sets key byte `+8` to `0xFF` at `0x420E34`, and returns with `ret 4`.
`0x4E4870` obtains a state object through `0x4EB0F0`, reads/writes its `+0x14`, updates
`seed = seed*214013 + 2531011` modulo 2^32, and returns `(seed >> 16) & 0x7FFF`
(`0x4E4875..0x4E489C`). The helper's internal lifetime was not disassembled here.

**MEASURED:** the 42 generated frames occupy 392,454 bytes in the prototype's pointer-free
header/pixel stream. Storing their identity alone changes the image after a fresh launch.
The experiment copies generated pixels into replay-owned memory and redirects only saved-scene
references; it neither writes the engine's random state nor replaces live engine assets.
One replay-owned generated-asset bank can serve every snapshot. The [file-backed scene test](../tadr-port/demo-recorder-exploration.md#6g-file-backed-scene-seeking-and-rendered-prefix-recovery)
verifies repeated packet replacement while one immutable bank stays alive until process exit.
Changing/freeing the bank and atlas key reuse still need a production lifetime/generation design.

### Replay snapshots and draw-created caches [SOURCE / DISASSEMBLED / MEASURED 2026-09-28]

The wider [recorder scene experiment](../tadr-port/demo-recorder-exploration.md#6d-wider-world-snapshots-and-camera-dependent-caches)
shows why an all-map copy of the renderer's existing fields is insufficient. These reads and
copy transformations were temporary research probes; no new engine byte patch was installed.

**Body orientation, DIS:** `DrawUnit 0x45AC20` obtains the unit from its second stack argument
at `0x45AC23`, exits when `unit+0x86` is nonzero (`0x45AC29..0x45AC2F`), and reads Object3do
from `unit+0x9E` (`0x45AC35`). Its three tests at `0x45AC3D..0x45AC7A` subtract each live
`unit+0x68/+0x66/+0x64` from cached `o3+0x1C/+0x1A/+0x18` in **16 bits**, sign-extend and take
the absolute value. Any magnitude at least 8 refreshes **all** three turns: dword at
`0x45AC82`, final word at `0x45AC90`. The same branch sets dirty `o3+8` at `0x45AC89`, clears
base-primitive `+0x26` at `0x45AC97`, and conditionally clears `o3+4` when primitive `+0x28`
bit 1 is set (`0x45AC9B..0x45ACA8`). Dirty is tested at `0x45ACB1`. The base-template copy
at `0x45ACDD` and piece-cache clears at `0x45ACEA..0x45ACF3` precede child/sibling reset calls
`0x45ACFE/0x45AD0E → 0x45B030`, compose `0x45AD1D → 0x45B0A0`, and dirty clear `0x45AD28`.
The cargo chain starts with `unit+0x8A` at `0x45AD2B`; its loop advances via `+0x8E` at
`0x45AE4A`. Afterward `0x45AE73` calls `0x458810` with the unit's Object3do and the draw
context `main+0x1437B`. Existing DrawUnit call sites are mapped in the sort-row section.

**MEASURED:** 302 of 398 live units in the final early snapshot had different live/cached
turn triples. Saved-copy normalization using that exact threshold, with live controls retaining
the original cached turns, restored the tested ground-unit headings at a newly visited camera.
This does not change the rule for reconstructing **already-composed** vertex buffers: use the
turns that actually composed them. Cargo and aircraft equivalence remain untested here.

**Composite allocation, DIS:** `0x458810` reads camera fields `main+0x1431F/+0x14323`
(`0x45881B/0x458821`), builds its draw-mode value from `unit+0x110` bit 29, `+0x10E` bit 0 or
`unit`'s vtable `+0x20` (`0x45883C..0x45886C`), and its rebuild condition at
`0x458870..0x4588F2`. The latter is detailed in the repose section below. On rebuild,
`0x4588FE..0x45890C` calls `0x4586A0(obj, 0, 1)` and clears cached shadow `o3+0x14`.
The builder reads `obj+0x0C` (owning unit) at `0x4586B8`, calls bounds builder `0x4581E0`
at `0x4586C9`, then chooses its allocation:

- nonzero second argument (`0x4586D2..0x4586D4`), `unit+0x114` bit 0
  (`0x4586D6..0x4586DD`), or a construction fraction unequal to zero
  (`0x4586DF..0x4586F0`) takes `0x458719 → 0x437BE0`, colour + depth;
- otherwise `0x458702 → 0x437B50`, colour only.

The comparison constant at `0x4FD4C0` is four zero bytes (**section bytes checked**).
The returned frame is stored through `obj+0x10`, null-tested at `0x458720`, and receives its
hotspot at `0x458729/0x458732`. The later `unit+0x110` bit 29 / `main+0x37F06` bit 5 tests
(`0x45873C/0x45874A`) concern the bake path, not that depth-allocation choice. The existing
cargo call `0x459670 → 0x4586A0(obj, 1, -1)` forces depth; this experiment did not exercise it.
No new disassembly of the allocators or bounds builder was needed; their existing map remains
the source for their internals.

**MEASURED:** visiting the two camera views while paused changed 200 of 398 units' captured
existing-depth flags. Recording `unit+0x114` bit 0, and supplying depth in the saved copy when
it or the build fraction required it, removed the shoreline discrepancy. The saved copy
supplied 255 depth flags, including units outside those views. `tagpu_native.c` uses the flag
with packet `sea_level` (publisher reads `main+0x1427F`) and owner/local-player/state-sonar
values for clipping/tinting. This is ordinary-unit evidence, not all cargo or mod variants.

**Animated features, SOURCE:** `tagpu_feat.c:draw_feature` resolves animation from live feature
or wreck records even with a saved anchor. Definitions have stride `0x100`, static body/shadow
sequences `+0xAC/+0xB0`, animation states `+0xCC/+0xD8`, and mask `+0xFE`. GAF wreck records
at `main+0x1420B`, stride `0x30`, instead use states `+0x04/+0x10` and shadow-enable flag
`+0x2F` bit 2. The probe bounds the definition index by the published feature count and wreck
index by `WR_COUNT`, resolves `tagpu_gaf_state_frame` / `tagpu_gaf_seq_frame` on the game
thread, and copies frame references into an expanded 24-byte anchor (normally 16 bytes).
These asset references live only through the current level/reclaim fence; they are not file
handles. The separate mirrored-feature consumer also resolves animation live and is not
covered by this on-map experiment. Whole-map feature/FX capture retains existing collector
bounds and reported no truncation in the measured packets; no maximum-capacity claim follows.

### Perspective authority and tick boundary [MEASURED / DISASSEMBLED 2026-09-28]

The [two-peer perspective experiment](../tadr-port/demo-recorder-exploration.md#6c-remote-perspectives-require-authoritative-contributions)
read the active-player slots (indices 0..9) at `main+0x1B63`, stride `0x14B`, restricting the live reads to
the two participating records. Within each: DPID `+4`, controller byte `+0x73`, LOS pointer
`+0x7C`, dimensions `+0x80/+0x84`, allocation length `+0x88`, and the 88-byte resource record
at `+0x8C`. The last field names come from Pascal's `TPlayerResourcesStruct`; the first eight
floats are energy current/production/expense, metal current/production/expense, and the two
storage maxima. `tagpu_scenario.c` independently uses `+0x8C/+0x98` for current energy/metal.
The economy pass `0x401360` and the save's Players loader `0x466050` (its key names in brackets)
confirm the layout [DISASSEMBLED 2026-09-28]: floats energy current/production/expense
`+0x8C/+0x90/+0x94` and metal `+0x98/+0x9C/+0xA0`, storage maxima energy/metal `+0xA4/+0xA8`;
doubles energy/metal produced `+0xAC/+0xB4`, consumed `+0xBC/+0xC4` and wasted `+0xCC/+0xD4`
(`TotalEnergyProduced` … `MetalWasted`); floats base storage `+0xDC/+0xE0`
(`PlayerEnergyStorage`, `PlayerMetalStorage`). After it: share limits metal/energy `+0xE4/+0xE8`,
the `0x34`-byte shared-resources record `+0xEC`, the 30-tick pass time `+0xF0`, kills `+0xFC`,
losses `+0xFE`, units created `+0x140` (dword) and alive `+0x144` (word), and the base-storage
flag `+0x149` bit 0 (*Replay state design*, below).

`main+0x14233/+0x14237` read 672/800; LOS width/height read 336/400, allocation 134,400 bytes.
The existing allocator map above establishes the in-level lifetime. With both games paused
(`main+0x38A51 = 1`) and no level transition, the reader requested only chunks within the
dimension-validated allocation. `main+0x14273`'s MAPPED pointer was read but its contents were
not copied in this experiment. GameTime `+0x38A47` pairs were 836/838 initially, 1620/1623 after
movement and on repeat, and 2273/2279 after adding 20 scouts. Matching DPIDs, one remote LOS
grid differed from its owner in 104 counter bytes, **34 zero/nonzero sight predicates**.
Each individual grid stayed byte-identical during the repeated paused read. Current resource
values/storage were present remotely but the measured production/expense fields were zero.
These are evidence for owner-contributed replay perspectives/statistics, not proof of every
field's authority or a reconstruction of radar/sonar/detection.

The `0x2C` clock was also rechecked in the pristine binary. Writer `0x48B710` calls the bit
writer `0x415C10` with 8-bit code `0x2C` at `0x48B72A`, a 16-bit length placeholder at
`0x48B737`, and **32 bits from `main+0x38A47`** at `0x48B743..0x48B74E`. It stores the same
counter at `player+0x18` (`0x48B760..0x48B769`). Receiver `0x48B920` reads those 8/16/32 bits
through `0x415DC0` at `0x48B93F/0x48B94A/0x48B955` and stores the third value at the sender's
`+0x18` at `0x48B963`. Thus the field Pascal calls a packet sequence is the sender's GameTime,
not evidence of one shared peer clock. The existing round-robin map was rechecked at
`0x48B80E..0x48B84C`: a 16-bit terminator precedes `GameTime % main+0x37EE6`.

`0x495490` was read through the loop's end as a potential owned recorder sampling boundary;
**no hook was installed**. It takes the iteration count from `main+0x38A3B`, increments
`main+0x38A47` at `0x4954C0`, and conditionally pumps `0x453D40` at `0x4954C8` when its argument
is nonzero. Per-iteration calls are `0x48AD30` at `0x4954ED` (already Impure's unit-tick hook),
`0x49B720` at `0x495513`, `0x420F30` at `0x495539`, `0x464F80` at `0x49555F`,
`0x424050` at `0x495585`, `0x415B30` at `0x49558A`, `0x490C40` at `0x49558F` (already the
wind hook), `0x437DE0` at `0x495594`, `0x41CA10` at `0x495599`, `0x471EB0` at `0x4955BF`,
and `0x466580` at `0x4955E5`. The intervening `0x4B6560` calls update timing accumulators in
`main+0x38D85` at offsets `+0x2C/+0x30/+0x34/+0x44/+0x48/+0x4C`.
When the argument and `[0x506DBC]` are nonzero, the tail takes player index `main+0x2A42`,
calls `0x457D30` on that record at `0x49563A`, then `0x4618A0` with context `0x513000` and
argument 0 at `0x495646`. The loop decrement/back edge is `0x49566C/0x49566D`. Calls to
`0x428BD0`, `0x428BE0`, `0x428BF0`, `0x463EF0` and `0x482130` at `0x495674..0x495688`
are **outside** that loop. Their meanings and complete side effects were not traced here.
An after-loop observer would collapse multiple iterations; a recorder promising every
transition must either observe inside the loop or capture the corresponding events.

### Replay player names and unit-block ordering [DISASSEMBLED + MEASURED 2026-09-28]

A native DirectPlay replay-host prototype supplies both DPNAME strings. The join path calls
`0x4CA7C0` at `0x450B6F`, reads the returned short-name pointer at `esp+0xDC` (`0x450B7E`)
and the long-name pointer at `esp+0xE0` (`0x450BA3`), and scans the latter unconditionally at
`0x450BAD`. A prototype supplying only the short name crashed at that scan with a null
long-name pointer. Non-null short and long names allow the viewer to reach the lobby.
The destinations are player `+0x49` and `+0x2B`, respectively.

Unit allocation constructs ten player pointers at `0x48562B..0x48564B`, sorts them, then
assigns each a contiguous range of `main+0x37EE6` units. It writes the first/last pointers
at player `+0x67/+0x6B` (`0x4858BD/0x4858E0`), the first/last slot words at `+0x6F/+0x71`,
and each unit's owner pointer and owner-index byte at `+0x96/+0xFF`
(`0x485902/0x48590E`). The loop does not omit spectators. A new viewer's DirectPlay ID
can sort before a recorded player: measured viewer ID 84525577 with drone IDs
84525579 and 84525580, despite creating twenty temporary players and retaining the lowest two.
Thus that heuristic does not preserve recorded unit ranges by construction.

All comparisons must use one ordering. The fixed ten-player path starts with the inlined
network comparison at `0x485676..0x48567E`, then invokes comparator `0x485940` through
`0x488920`. Further inlined network comparisons are at `0x4857AF..0x4857B7` and
`0x485847..0x48584F`; they lie on the larger-sort branch, whose initial size test is
`0x28 > 0x40` and is false for ten players. `0x485940` tests game type through `0x435100`;
its type-3 branch compares player `+4` IDs at `0x485959..0x485967`, while the other branch
compares player pointers (`0x48596A..0x485972`). Both return unsigned less-than.
`0x488920`'s insertion loop relies on the first element already being the minimum: after
`0x48894C` calls the comparator it shifts backward without a separate lower-bound check.
Changing its comparator without changing `0x485676` violates that ordering invariant and
can walk before the array; the isolated probe reproduced that failure. It is not a stock
engine defect.

A revised isolated probe uses immutable recorded-player ranks at all four network
comparisons while leaving transport IDs unchanged. It loaded the replay with drone
85151720 at slot 1, drone 85151721 at slot 1501, and spectator 85151724 at slot 3001
(unit-array base `0x118A0020`; first pointers `0x118A0138`, `0x119069D8`, `0x1196D278`).
This is a two-player experiment, not a production ordering patch or ten-player coverage.

Disassembly ranges: `0x450B60..0x450BC0`, `0x48562B..0x485975`, and
`0x488920..0x488960`, using `objdump -D -Mintel` against the pristine retail binary.

The stock save dialog can be exposed in the isolated network viewer by changing only the
UI type-3 comparisons at `0x460CF7/0x460D37`; that does not enable a valid restore. A measured
369-unit spectator save wrote 392,041 bytes, and loading it displayed “Invalid savegame file.”
The load action `0x492360` opens the selected summary through `0x432520` (`0x4924A7`), reads
`Gametype` via `0x4B4800` (`0x4924C0`, key string `0x504974`), and accepts only values 1 and 2
by successive decrements (`0x4924CD..0x4924D3`). Other types branch to `0x492AAD`; the
error text at `0x509310` is displayed at `0x492AE2`. A later read at `0x49267F..0x492690`
passes the saved game type to `0x434AB0`. Thus changing only the menu's disabled state is
insufficient. The temporary restore probe admits exactly type 3 at the rejection branch;
no claim about complete restored state follows from bypassing that UI validation.

With the type-3 admission probe, loading a fresh 399,676-byte spectator save crashes after
level teardown at `0x46E167` (`mov eax,[ebp+0x64]`, `ebp == 0`). The stack includes return
`0x46CA75`: `0x46CA60` loads `main+0x2A30` into `ecx` and calls `0x46E160` at `0x46CA70`
without a null check. Its callers are `0x428345`, `0x4283BC` and `0x42845D` (E8 scan, no stored
pointer), cases of the menu state machine's switch on `main+0x2BBF` (`0x4282D5..0x4282EB`);
`0x42845D` ends the case `0x428439..0x42845B`, which first resets every type-4 record (remote AI)
to 0 through `0x463C60(0)`. This is the restriction/sync object identified
in the unit-cap investigation above. The observed missing object is a restore-lifecycle gap;
there is no proposed null-skip fix or proof of successful multiplayer restoration.
Additional slices: `0x46E160..0x46E1D0`, `0x46CA60..0x46CAD0`, `0x428439..0x428478`.

The console save action `0x417430` builds its filename and calls writer `0x4326B0` at
`0x41747B`; its argument-count test is `0x41743A`. The writer begins a HAPIBANK object via
`0x4B3620/0x4B3750` and opens the initial account with `0x4B4560` (`0x4326B6..0x4326D3`).
The measured save starts with `HAPIBANK` and contains `SQSH` chunk markers. This confirms
container framing only, not a decoded checkpoint schema. Additional disassembly ranges:
`0x417430..0x417489`, `0x4326B0..0x43274E`, `0x460CC0..0x460D60`,
`0x492260..0x492730`, and `0x492A20..0x492B05`.

The full writer `0x4326B0..0x432A7F` serializes the game tick at `0x432997..0x4329AC`.
Its main content calls are `0x4C6F10` (argument from `main+0x142DB`), `0x41D360`,
`0x4662F0`, `0x4876C0`, `0x484F50`, `0x424890`, `0x484DF0`, `0x484CE0`,
`0x438180`, and `0x48FDF0` at `0x4329E3..0x432A48`. They are skipped when
`main+0x391F1` is nonzero (`0x4329B7..0x4329BD`). Their complete transitive state
coverage is not established by this caller inventory. `0x48FDF0` runs only for game type 1
(`0x48FE06..0x48FE09`) and invokes vtable slot `+0x10` across two object arrays/counts
at its receiver `+0/+0x40` and `+0x44/+0x84`; it returns without doing so in a type-3 save.
The arrays' object meanings remain unestablished. Additional slice: `0x48FDF0..0x48FE59`.

### Replay state design: remote-player gates, rebuild paths and player state [DISASSEMBLED 2026-09-28]

The engine facts the state-based replay design rests on: what the engine runs and skips for a
remote (type-3) player, which stock paths rebuild units, orders and queues in place, and where
per-player and perspective state lives. The reasoning, the design consequences and the open
experiments are in the [state design evidence](../tadr-port/demo-recorder-state-evidence.md):
[puppet mode](../tadr-port/demo-recorder-state-evidence.md#1-puppet-mode-how-the-engine-treats-a-remote-player),
[rebuild and takeover](../tadr-port/demo-recorder-state-evidence.md#2-rebuild-and-takeover-inventory),
[player and perspective state](../tadr-port/demo-recorder-state-evidence.md#3-player-and-perspective-state)
and [the size estimate](../tadr-port/demo-recorder-state-evidence.md#4-size-estimate).

| Address | What it is | The fact |
|---|---|---|
| `0x463C60` | the controller-type setter, `thiscall(type)`, `ret 4` | Stores `+0x73` and, for any type but 3, PlayerInfo `+0x94` too (`0x463C60..0x463C75`). Fifteen callers (E8 scan): the `AI` command (`0x4162F4`, `0x4162FE`), the type-4 reset `0x42844F` (above), the seat removal `0x452E62`, and `0x4453E6`, `0x44552E`, `0x447C79`, `0x447CC2`, `0x447CFA`, `0x44833D`, `0x44A8BF`, `0x45125E`, `0x451442`, `0x45401B`, `0x454054` [roles not traced]. DIS |
| `0x416280` | `AI`, the chat command `{0x5018E0 "AI", 0x416280, 4}` at `0x501FD0` (run level 4) | Takes a seat below 10 (`0x4B73E0`, `0x416293..0x4162A0`) whose record is active (first dword ≠ 0) with `+0x73` 1–3 and `+0x146` ≠ 10; type 1 becomes 2 (`0x4162F4`), types 2 and 3 become 1 (`0x4162FE`). It touches no unit, mover or AI object. DIS |
| `0x48AD30` | the unit tick, over each record's block `[+0x67, +0x6B]` | Ticks only active records with `+0x73` 1–3 and `+0x146` ≠ 10 (`0x48AD6C..0x48AD9C`): a type-4 record's units are never ticked. Every ticked unit runs `0x437910` (`0x48ADC4`), `DoScriptsNow(1)` when `+0x9A` is set (`0x48ADDF..0x48ADEB`), its countdowns `+0xFA`/`+0xFB` and 30-tick health percentage `+0xF6`/`+0xF7`, and the pending-death reaper (`+0x110` bit 14 → `0x4864B0(unit, +0xF5)`, `0x48AFB9..0x48AFD1`). `AutoAim 0x49E1A0` needs the block owner's record (`edi`, `0x48AD73`) active and of type 1 or 2 (`0x48ADC9..0x48ADDA`). Only a unit whose owner `+0x96` is of type 1 or 2 (`0x48AEB5..0x48AECD`) runs the kind-`0xB` hit every 30 ticks at or below the sea level `main+0x1427F` for a def without `+0x241` bit 12 (`0x48AED3..0x48AF32`) [INFERRED: water damage], `0x41BD10` every 8 ticks while HP is below def `+0x1FA` and def `+0x200` is set (`0x48AF37..0x48AF92`) [INFERRED: regeneration], `0x43B7C0`, `0x43BAD0` and the movement pass `0x43DD20` + `0x48A870` (`0x48AFA3..0x48AFB0`). After the block's units, the `0x2C` sender `0x48B710` runs for an active type-1/2 record in a network game (`0x48AFE4..0x48B003`). **The slot loop** reads the block's last slot once (`0x48ADA2`) and tests each slot's type index `+0xA6` only when it reaches it (`0x48ADB0..0x48ADB8`; the step `0x48AFD6..0x48AFDE`), so a unit created during an earlier turn of the same tick, in a later slot of a block not yet finished, gets its turn and its script slice in that same tick. **A unit's turn and its COB**: `DoScriptsNow(1)` (`push 1` at `0x48ADE9`, the call at `0x48ADEB`) is the only call of `0x4B0D60` in `.text` and the only `dt` a sleep counts down by (*The eight records*, below), and it runs before the unit's orders and movement pass. A run-later start made in those, StartBuilding at `0x4385C7` (in `0x438590`, called from the build orders) or StopBuilding at `0x43A251` (in the order destructor `0x43A1F0`), both `0x4B0B00` with run-now 0, takes its first slice at the unit's next turn, unless the same turn's walk class makes a run-now start (`0x43DAF2`/`0x43DB27`), whose pass (`0x4B0940`'s run-now branch `0x4B09C4..0x4B09E8`: the runner `0x4B0DA0` over records 0-7, then the stepper `0x4B1C00`, both with `dt = 0`) runs every record with `dt = 0`. The movement pass's one call of the walk class `0x43DA70` (`0x43DD5D`, the only call in `.text`) starts StopMoving or StartMoving (`push 1` at `0x43DAD5`/`0x43DAE3`, the call at `0x43DAF2`) and then MoveRate1..3 (`push 1` at `0x43DB02`/`0x43DB0D`/`0x43DB18`, the call at `0x43DB27`) through `0x4B0940` with run-now 1, which runs every record with `dt = 0` inside the start. So a level-start unit the loader makes before the first tick has had one slice by that tick's end, which is why a playing makes the level's units at the first iteration's capture point, before this call (*COB on a puppet*, below). **The script call's site** (`0x48ADDF..0x48ADF0`: `mov ecx,[esi+0x9A]; test ecx,ecx; je 0x48ADF0; push 1; call 0x4B0D60`, 17 bytes): the only branches to `0x48ADDF` are AutoAim's skips `0x48ADCC` and `0x48ADD7`, nothing branches into `0x48ADE0..0x48ADEF` (a rel8/rel32 scan of the whole file) and no dword names an address there; at `0x48ADF0` the flags are dead (`mov al,[esi+0xFA]`, then `0x48ADF6 test al,al` sets them), and so are `eax` (reloaded at `0x48ADF0` and `0x48AE02`), `edx` (`0x437910`'s call clobbered it on every path) and `ecx` (the COB or DoScriptsNow's leftovers), while `esi` (the unit), `edi` (its record), `ebx` and `ebp` are live. Impure jumps the 17 bytes to a stub that gives the slice from C (`tagpu_patches.c`, THE SCRIPT SLICE; *The COB module's entries*, below). DIS |
| `0x464F80` | the players phase, once a tick (`0x49555F`) and once from the loader body `0x497180` on the loader thread (`0x497C10`) | The unit tick's record gate (`0x464FCE..0x465024`). Every tick, per record: `0x408C40` on `+0x74` when set (`0x46502A..0x465031`), `0x40B2C0(i)`, i the record's slot (the loop index, pushed at `0x465036`), and the sight emit `0x4827B0` for each live unit (`+0x110` bit 28) of the record's own block `[+0x67, +0x6B]` (`0x46503C..0x465063`); for the viewed player `main+0x2A43`, the minimap rebuild `0x466DC0` (`0x465065..0x465072`). On the record's 30-tick pass (`0x46507D..0x465092`) the viewed player's record also runs the radar pass `0x467440` (its one call, `0x46556D`) and `0x466C20` (`0x465572`), and in a network game every fourth such pass (counter `0x51E53C`) sends its `0x28` through `0x4573D0(rec, 0, 0)` (`0x4655A1`). The economy pass and the network defeat: *The Deathmatch respawn*, below. DIS |
| `0x408C40` | the tick of the `+0x74` AI object (`0x3D` bytes; its constructor `0x408CB0` stores the record at `obj+0`, `0x408CBB`, and its `+0x146` at `obj+4`) | Plans only when that record is active with `+0x73` = 2 (`0x408C45..0x408C50`): `0x408830` every 30 calls (countdown `obj+5`), the ten slots at `obj+0x11` when due, then `0x4089A0(1)`; any other type runs only `0x4089A0(0)` (`0x408CA7`). A seat flipped 2 → 3 keeps its object, dormant, and a flip back to 2 revives it. DIS |
| `0x40B320` / `0x40B2C0` / `0x40B390` | the second per-seat object, in the array `0x5119C0` | `0x40B320(idx)` allocates `0x10D` bytes, constructs them with `0x409160` and stores the pointer at `0x5119C0[idx]` (`0x40B340`); `0x40B390(idx)` frees it. `0x40B2C0(idx)` returns when `0x5119C0[idx]` is NULL (`0x40B2CC`); otherwise, once 30 ticks have passed since the GameTime it keeps at `obj+0xED` (`0x40B2D6..0x40B2E5`), it runs the acquisition `0x40AA40`, draws the sim RNG `0x4B6C30(0x1E)` (`0x40B301`) and calls `0x409730` when the draw is 0 (`0x40B30C`): a seat with this object draws the shared sim RNG once per 30 ticks. The player init `0x464700` builds it beside `+0x74` for every seat but an active type 3, and the teardown's `0x464A00` frees both (*The LOS counters and MAPPED both end in the teardown, the counters first*, below). The init and the teardown index `0x5119C0` by `+0x146` (`0x4648C6..0x4648CD`, `0x464A48..0x464A4F`) and the players phase by the slot (`0x465036`): the same object while `+0x146` equals the slot [INFERRED that it does for every ticked record: its writers store the slot or 10, *Which bytes are read back and which are not*, below]. **`+0xED` starts at 0**: the constructor `0x409160` zeroes `ebx` (`0x40918B`) and stores it there (`0x4092DC`), and the only other stores are the two scan paths' GameTime after their scan (`0x40AD51`, `0x40B2FB`), so a record made at GameTime 30 or later scans at its first players phase; the demo recorder's takeover relies on it (*The radar pass `0x467440` and the minimap's fog composite*, below). `0x40AD20` is a second copy of the gated scan (`0x40AD29..0x40AD3E`, the same 30-tick test, then `0x40AA40` and the RNG draw) with no direct call and no pointer to it anywhere in the file. DISASSEMBLED 2026-10-11 for `+0xED` and `0x40AD20`. DIS |
| `0x485E50` | a mover install that nothing calls (no E8/E9 caller, no stored pointer) | `0x4B4F10(0x2F)`, `0x43DC00(unit)` (`0x485E66`), stored at `unit+0` (`0x485E6F`), `unit+0x66` = def `+0x210`, `ret 4`; it does not free the old mover. `0x43DC00`'s other call sites are the two creates (`0x4860D5`, `0x4862E9`) and the dead `0x485E90` (`0x485F29`), and `0x43DD10` frees a mover from the destructor only (`0x486DB1`): no stock path swaps a live unit's mover, whose class the owner's `+0x73` chose at the create. DIS |
| `0x48BA28..0x48BA5C` | the `0x2C` receiver's block sweep | After the dirty list, every unit of the sender's block `[+0x67, +0x6B]` with `+0x110` bit 28 (alive) and a mover runs `0x43DD20` (`0x48BA46`) and `0x48A870` (`0x48BA4C`), once per message: a remote unit's mover steps, and its walk scripts start, only when its owner's `0x2C` arrives (*The move object*, below). DIS |
| `0x455F84` entries `0x455FB0`, `0x455FB4`, `0x455FC0`, `0x455FC4`, `0x455FC8` | the event cases `0x0D` `0x45542D`, `0x0E` `0x45543D`, `0x11` `0x45555B`, `0x12` `0x4555BA`, `0x13` `0x45561A` | Plain calls of stock functions. `0x0D` → `0x49D270(sender, msg)`, which never reads its sender and rebuilds the shot through `0x49C740` (`0x49D307`, `0x49D4FC`), `0x49CDE0` (`0x49D44E`), `0x49CC20` (`0x49D479`) or `0x49C9C0` (`0x49D558`), the holders of the `Fire*` starts (`0x49CB94`, `0x49CD4F`, `0x49CF73`); its retarget search under `msg+0x1A` bit 0 skips projectiles whose owner `+0x66` is `main+0x2A42` (`0x49D3D8..0x49D3E1`). `0x0E` → `0x49AF90`, which never reads its sender. `0x11`: an alive test (`0x455585`), then `0x48B090(mask, 1)` and `(~mask, 0)`. `0x12` → `0x41B8D0(builder, built)` (below). `0x13`: with `msg+1` set `0x47F0C0([+2], 0)`, else the positional `0x47F300([+2], &msg[6], 0)`. DIS |
| `0x4554EE..0x455556` | the `0x10` script-start case (entry `0x455FBC`) | The message is `{0x10, u16 unit, i16 script, u8 argc, 4 × i32}`, `0x16` bytes. Unit 0 is taken as NULL and then read at `0x455518`, a fault; the unit index has no other bound (`0x4554FF..0x455516`). After the alive test it calls `0x4B0B00(unit+0x9A, script, 0, 0, argc, a0..a3)` (`0x455551`) with no null test of `+0x9A`, so the script steps at the unit's next `DoScriptsNow`. The script index is bounded there: `0x4B0B00`'s `COBEngine_AllocThread 0x4B08C0` (`0x4B0B08`) returns −1 below 0 or at or above the script count `[[cob+8]+4]` (`0x4B08C5..0x4B08D0`), and then nothing starts (`0x4B0B0D..0x4B0B22`). The senders `0x456190` (called at `0x438632`, `0x43A258`), `0x456200` (`0x411403`, `0x49E3A6`; the index through `0x4B07C0`) and `0x456290` (`0x4385D7`) send only with `main+0x2A44` bit 0 (`0x456198`, `0x45621E`, `0x456298`). DIS |
| `0x4573D0(rec, target, flag)` (`ret 0xC`) / `0x457540` | the `0x28` resource message, sent and received (case `0x4558FE`, entry `0x45601C`) | Sent only for an active type-1/2 record with `+0x22` = 0 (`0x4573DA..0x4573F7`): 58 bytes, `[1]` the flag, `+0xFC`/`+0xFE`/`+0x104`/`+0x106`, current metal `[0x12]` and energy `[0x16]`, storage `+0xA8` `[0x1A]` and `+0xA4` `[0x1E]`, and the six totals as floats (`0x4573FD..0x45749C`), never production or expense. A NULL target sends to every active type-3 record whose PlayerInfo `+0x94` is 1 and `+0x22` is 0 (`0x4574CA..0x45752B`). The receiver needs a sender whose `+0x22` is 0, skips the writes when any active local record's byte `+0x129 + sender` is set, writes those fields (`0x4575B7..0x457637`) and answers a set flag with a `0x29`. DIS |
| `0x464C60(from, to, amount, send)` (`ret 0x10`) | the metal transfer | Returns at once when either index is 10 (`0x464C67..0x464C77`). With `send` ≠ 0 it caps the amount at the sender's current metal (`0x464C87..0x464CB6`); whatever `send` is, it returns when the amount is then 0 (`0x464CBA..0x464CC9`). With `send` ≠ 0 it takes the amount through `0x401260` (`0x464CF4`; current −= amount, the `+0xEC` record's `+0x1C` += amount). In every case it adds the amount to the recipient's `+0xEC` record `+0x18`, which the economy pass folds in; a type-2 recipient gets 50 % at difficulty 0 and 70 % at 1 (`0x464D3C`; −0.5 at `0x4FD548`, −0.7 at `0x4FD540`). With `send` ≠ 0 it then sends through `0x457050` (`0x464D85`). The network case `0x4558D6` passes 0 (`0x4558CA`), so a received transfer credits the recipient and debits nobody on the receiving peer: the debit happens on the sender's peer only. The auto-share `0x457E78` passes 1 (`0x457E73`). Callers `0x416C8D`, `0x4558D6`, `0x457E78`, `0x49362C`. DIS |
| `0x457D30` | the auto-share, called from the tick loop for `main+0x2A42` (`0x49563A`, its one call) | Needs `main+0x2A44` bit 0 and runs every 60 ticks; with PlayerInfo `+0x97` bit `0x2` and current metal above `+0xE4` it gives metal through `0x464C60` (`0x457E78`), with bit `0x4` and energy above `+0xE8` energy through `0x464B30` (`0x457F96`). DIS |
| `0x418CD0`, `0x418D90`, `0x418E50`, `0x418FD0`, `0x419090`, `0x419340`, `0x419400` | the share commands of the level-1 table `0x501D38` (entries `0x501EB8..0x501F0C`) | ShareMetal, ShareEnergy, ShareMapping and ShareRadar toggle bit `0x2`, `0x4`, `0x20` and `0x40` of the controlled player's PlayerInfo `+0x97` (`0x418D3E`, `0x418DFE`, `0x418EBE`, `0x41903E`), ShareAll all four, each then calling `0x450F90`; `0x418F10` toggles `0x08` (`0x418F7E`) and nothing references it. SetShareMetal and SetShareEnergy write `+0xE4` (`0x4193C5`) and `+0xE8` (`0x419485`). The auto-share reads `0x2` and `0x4`, the radar pass `0x40`; a reader of `0x20` or `0x08` is open. DIS |
| `0x467440` | the radar pass, for the viewed player `main+0x2A43` | Five loops over the unit array, one back edge each (`0x46750C`, `0x4675CC`, `0x46766C`, `0x4676FD`, `0x467829`). **1** (`0x46749F..0x46750C`) clears bit 12 of every live unit (`0x4674AE`), sets `+0x110` bits 8–9 on the viewer's own units, on allied units whose owner shares radar (`AllyFlagAry[viewer]`, `0x4674C5`; PlayerInfo `+0x97 & 0x40`, `0x4674D2`) and on every unit when the viewer has the watcher bit (PlayerInfo `+0x9B & 0x40`, `0x4674E4`), and clears bits 8–10 of the rest. **2** (`0x46750E..0x4675CC`), radar and sonar: for each unit of the viewer's own block (`rec+0x67..+0x6B`) that is live, without `+0x110` bit 14, activated (`+0x10E` bit 0) and has a radar `def+0x204` or a sonar `def+0x206`, the bucket walk `0x47E890` (`0x4675B8`) with the visitor of vtable `0x4FD55C`. **3** (`0x4675EC..0x46766C`), jammers: each live, activated unit of another owner with `def+0x20A` or `def+0x20C`, the walk with the visitors of `0x4FD558` and `0x4FD554` (`0x46762C`, `0x467655`). **4** (`0x467690..0x4676FD`): for a unit whose owner record's `+0x73` is 1 or 2 and a def with `+0x245 & 0x2000`, `0x40B0D0` (`0x4676C8`); on true, `+0xB0` = GameTime + 90 and bit 12. **5** (`0x46770D..0x46782F`), the sight loop: bit 8 from the viewed record's sight counters or its explored-map bit (*The puppet host: … the sight loop*). Its one call is `0x46556D` (the only `call 0x467440` in the listing, and no pointer to it anywhere in the file, DISASSEMBLED 2026-10-11), on the viewed player's 30-tick pass, in the players phase after the unit tick: in step P it reads image P+1's units and the globals mid-step, so no image reproduces it (*The radar pass `0x467440` and the minimap's fog composite*, MEASURED 2026-10-09). The visitors write bits 8–10 alone: `0x467840` sets bit 9 or 8 (`0x467905`, `0x46793D`), `0x467960` and `0x467980` clear bit 8 or 9 and set bit 10 (`0x46796A`, `0x46798A`). **The sight loop's cell**: column = the high word of x `+0x6C` `>> 5`, row = (the high word of z `+0x74` − (the high word of y `+0x70` `>> 1`)) `>> 5`, a 32-unit cell, under true LOS (`0x46776B..0x467783`) and against the explored map (`0x4677B1..0x4677C9`) alike. **The pass of step P misses what step P does after its players phase**: a unit made or killed later in P — by the scenario applier at the frame tail `0x4969D2`, or the SHARE give's create at `0x488700` between two steps — keeps the create's bits (`0x485C1D..0x485C48`: bit 9 for the viewed record's own, nothing else) until the pass of P+30. MEASURED 2026-10-23 on a recording whose waves are made at the frame tail of step 540: the viewed seat's 8 ARMPW hold `+0x111` = `02` at images 541 and 570 and `03` at 571, the AI seat's CORSOLAR `00`, `00` and `01`; a playing, whose step 540 plays those creates before the host's players phase, gives them bit 8 at 541 (the demo recorder's THE RECORDED VIEW'S CONTACTS, below). DIS |
| PlayerInfo, `[player+0x27]` | the `0xB9`-byte block the record's constructor `0x463BE0` allocates | `+0x94` (written by `0x463C60`, read by the `0x28` send), `+0x95` side (read at `0x48692A`), `+0x96` colour (an index into `[main+0x148DB] + i·8 + 0x28`, `0x467C0E..0x467C24`), `+0x97` the share bits, `+0x9B` bit `0x40` the watcher bit (set by the network defeat at `0x46569D`, at `0x427F40`, `0x42814A` and `0x443BF3` on the controlled player `main+0x2A42`, and at `0x447F0E` on an active type-1 record; cleared at `0x4439D3` and `0x443C41` on the controlled player and at `0x447E8E` [the callers of these not traced; the word's other stores not checked for this bit]; read at `0x465154`, `0x4674E4`, `0x41DD43` among others). The Players loader restores `+0x95` (`Side`) and `+0x96` (`Logo`). DIS |
| `0x4816A0(arg)` `Game_SetLOSState` | the sight rebuild | Nine callers: `LOSType` `0x4166B1` (arg 0, after writing LosType bit 2), `LOS` `0x416D76` (0, bit 1), `Mapping` `0x416DA6` (1, bit 0), `NowISee` `0x41755E` (1, bits 0 and 1 cleared), the Deathmatch respawn `0x4654EF` (1), the network defeat `0x4656CE` (1), a screenshot routine (`0x495B61`, `0x495E58`) and the level load `0x497AE5` (1). With arg ≠ 0 it refills the whole explored map `*(main+0x14273)` (`0x4816A7..0x4816FB`); it refills the sight counters of each record 0..9 that is active, of type 1–3 and not numbered 10 at `+0x146` (`0x481707..0x48177A`; *The demo recorder's globals, projectiles and seek* has the rest), re-emits the units under LosType bit 1 (`0x48177C..0x481836`), clears LosType bit 3 (`0x481911`) and calls `0x466C20` and `0x466DC0`. **It does not run the radar pass**, and nothing on its path writes `+0x110`: neither its body nor the stamp helpers it calls (`0x481930`, `0x482270`, `0x4825B0`, all inside `0x481930..0x4827B0`) touch `+0x110..+0x113`, `0x466C20` makes no call and no `+0x110` access, and `0x466DC0` only reads `+0x110`/`+0x111` in its body. So a typed LOS change leaves the contact bits 8–10 as the last radar pass set them, until the next (DISASSEMBLED 2026-10-11; MEASURED on tenseat, LOS typed at GameTime 900: images 901..930 hold LosType 6 with a LosType-4 pass's bit 8, which the pass of step 930 clears on the units the viewed record's sight does not cover). DIS |
| `0x484FA0` | the saved game's `Mapping` loader | Accepts the section only at the explored map's own size, `w·h/2` bytes of `main+0x14233`·`main+0x14237` (`0x484FCD..0x484FE6`), and reads it straight into `*(main+0x14273)` (`0x484FE8..0x484FF7`). DIS |
| `0x466050` | the saved game's `Players` loader | Reads `Human Player` into `main+0x2A42` and `+0x2A43`, `GameTime` (`0x1C` bytes) into `main+0x38A37`, and per `Player%i` `Energy`/`Metal` into `+0x8C`/`+0x98`, the six totals, `PlayerEnergyStorage`/`PlayerMetalStorage` into `+0xDC`/`+0xE0`, `AddPlayerStorage` into `+0x149` bit 0 (`0x4661E7..0x466200`), `Kills`/`Losses` into `+0xFC`/`+0xFE`, `UpdateTime`/`WinLoseTime`/`DisplayTimer` into `+0xF0`/`+0xF4`/`+0xF8`, `Logo`/`Side` into PlayerInfo `+0x96`/`+0x95` and `Alliances`, 11 bytes, into `+0x108`; a missing `Player%i` sets `+0x73` = 0 (`0x4662C3`). It restores no production, expense, `+0x104`/`+0x106` or `+0x140`/`+0x144`. DIS |
| `+0x149` bit 0 | the base-storage flag (`AddPlayerStorage`) | Every instruction with that displacement, and `main+0x1CAC` at `0x497114`: the one test, `0x401941` in the economy pass; the sets by `0x496E90` (`0x496EA1`; callers `0x465F42`, `0x46540F`), by the skirmish seat setup `0x496EE0` (`0x496F5D`), at `0x497114` in `0x497080` (no E8/E9 caller, no stored pointer) and by the network setup after the commander's create (`0x4977D8`), each storing max(value, 200) into `+0xDC`/`+0xE0`; the clears by the player init (`0x4647EF`) and a commander's death (`0x486518`); the Players loader (`0x4661E7..0x466200`) and the saver (`0x466487`). No create sets it. DIS |
| `0x41DC20` | the post-game table, called by the teardown at `0x491B8B`, before `0x464A00` frees the records' objects | Takes the win flag `main+0x391AF` from `main+0x3923B` bit 4. A record with `+0x22` = 0 gets a row when it is active, of type 1–3, `+0x146` ≠ 10 and without the watcher bit, or has created units (`+0x140` ≠ 0) (`0x41DD13..0x41DD5B`): the name `+0x2B`, kills `+0xFC`, losses `+0xFE`, the produced `+0xAC`/`+0xB4` and wasted `+0xCC`/`+0xD4` totals as integers and a score (`[main+0x391E9]` `+0xD58`, `+0xD54`), into rows at `main+0x38DD9`, with column maxima at `main+0x3918F..+0x391A7`. ENDMSN shows the live records as the teardown finds them. DIS |
| `0x43B0B0(action, unit, type, amount)` (`ret 0x10`) | a factory queue as count nodes | For amount > 0 it adds to the last node of the list the action's descriptor picks (`+0x60` for its flag `0x40000` in `*(0x512344)`, stride `0x19`, else `+0x5C`) when that node's action byte `+4` and `+0x36` match (`0x43B102..0x43B10C`, into `+0x3A`), and otherwise appends through `0x43ADC0(action, 1, unit, 0, 0, type, amount)` (`0x43B116..0x43B120`); for amount ≤ 0 it subtracts from the last matching node, destroying a node it empties and carrying the rest to the next match (`0x43B12C..0x43B1DC`). A queue A, B, A is three nodes. Its only callers are `0x419B8C` and `0x419BA8`. DIS |
| `0x419B00(name, unit, amount)` (`ret 0xC`) | the build-menu queue call (callers `0x41AC3C`, `0x40865E`, `0x4087C4`) | Plays `addbuild` or `subbuild` (`0x47F1A0`) when the unit's `+0xFF` is the viewed player (`0x419B10..0x419B33`). `MAKENUKE` and `MAKEANTI` queue `BUILDWEAPON` with type 0 (`0x419B97..0x419BA8`); any other name is resolved to a type (`0x488B10`; an unknown one queues nothing) and queued as `MOBILEBUILD` for a unit with a mover, else `BUILDINGBUILD` (`0x419B6B..0x419B8C`), the action resolved by name through `0x438760`. The click at `0x41ABFA..0x41AC3C` passes ±1, or ±5 with SHIFT (`0x4C1B80(0xF9)`). DIS |
| `0x402640(unit, order, event)` (`ret 0xC`) | the `BuildingBuild` order's handler (action `0x0C`), its pointer at `0x4FC55C` (record `0x4FC558`; *The nano spray*, below) | Event bit `0x2` is tested first (`0x402647..0x40264D`). With a target `+0x16` it credits the target def's `+0x18A` × `(1.0 − target+0x104)`, truncated to an integer by `0x4E43A0` (`0x40267E`), to the unit's `+0xD4`; a type-2 owner (`unit+0xEC`) gets 50 % at difficulty 0 and 70 % at 1 (`0x402687..0x4026E3`; −0.5 at `0x4FC6E0`, −0.7 at `0x4FC6D8`). It then calls `0x41B8D0(unit, target)` (`0x4026EE`) and `0x489BB0(unit, target, 0x7530, 9, 0)` (`0x402701`). With or without a target it calls `0x48B090(9, 0)` (`0x40270C`) and `0x41C150(unit)` (`0x402712`) and returns 5. Event bit `0x8` ("Construction stopped", `0x501340`) decrements the node's count `+0x3A` (`0x40272A..0x402751`). Otherwise it dispatches on the state `order+5` through `0x402B5C`: state 0 needs `+0x110` bit 29 (`0x40277B..0x402785`) and keeps the unit activated (`0x48B090(1, 1)`) while `+0x3A` > 0, deactivating it and returning 5 at 0 (`0x40278B..0x4027B3`); state 4 (`0x402B07..0x402B39`) completes one unit: `0x41B8D0(unit, order+0x16)`, the target link cleared, `+0x3A` decremented. The unit in progress is the order's target `+0x16`. DIS |
| `0x41B8D0(builder, built)` | the build completion (callers `0x4026EE`, `0x402B25`, `0x41BCBF`, `0x4555F1`, `0x455610`) | Calls only `0x4AB060`, `0x49FA90`, `0x48AAC0`, `0x48B090`, `0x4199B0` (which sums queue counts through `0x439D80`) and `0x4560C0`, and links no order to the built unit: how a built unit takes a factory's rally orders is open. DIS |

Described where they live: the resource layout (*Perspective authority and tick boundary*, above);
the economy pass `0x401360` and the network defeat `0x465643` (*The Deathmatch respawn*); the load
barrier's network-only path (*The load state `0x497F40`*); the movement pass `0x43DD20` and the
movers' steps (*The move object `0x43DC00`*); the stamp's eviction of a type-3 incumbent (*Slot B*);
the player init `0x464700` and the teardown's `0x464A00` (*The LOS counters are allocated in the
level load and nowhere else* and the paragraph after it); the order links (*The order module*); the
save's per-unit sections (*A saved game's unit records*); what a create skips without `finished`
(*What one local create sends*); the dispatcher's sender test (*A sender, as the dispatcher sees
it*); and the send's network and local-record gates (*The send `0x451DF0`*).

### Driving and rebuilding puppets: pacing, the movement pass's parts, the mover's layer, adoption and the event senders [DISASSEMBLED + MEASURED 2026-09-29]

What a puppet host must do every tick, what a rebuild in play runs into, and what a skirmish
recorder needs from the event senders. Read and measured for the demo recorder's experiments 2 and
3 with a temporary probe build (not landed; Wine, a private display); the runs and what they mean
for the design are in the state design evidence,
[*Recording and replay, measured*](../tadr-port/demo-recorder-state-evidence.md#recording-and-replay-measured)
and [*Rebuild, takeover and rewind, measured*](../tadr-port/demo-recorder-state-evidence.md#rebuild-takeover-and-rewind-measured).

- **The pacing gate `0x495230`** decides how many ticks a frame runs, in both branches of the frame
  function (`0x4967F3` in a network game; `0x496928` otherwise, skipped while `main+0x37EBE` bit 0
  or the pause bit `main+0x38A51` bit 0 is set). Among the active records with type `+0x73` = 3 and
  a live count `+0x144` above 0 it takes the smallest `+0x18` below GameTime
  (`0x4952CF..0x495303`) and stores that record at `main+0x511` and the lag at `main+0x515`
  (`0x49530D`). From a lag of `0x384` (900) ticks it clamps the lag at `0xE10` (3600), scales the
  due ticks by (3600 − lag) × 1/2700 (`0x4FDA30`), floored at 0.01 (`0x4FDA38`), and sets
  `main+0x38A51` bit 1 (`0x49535C`); below 900 it clears that bit (`0x495367`). Before that, on
  every call, it stores bit 2 as the live speed `main+0x38A4D` below the chosen `main+0x38A4B`
  (`0x49526F` `cmp`, `0x495281` `sbb`, `0x495283..0x495299`), so bit 2 is set while its own trim
  holds the speed down. The due ticks go to `main+0x38A3B`, at most 5 (`0x4953F5`). **A puppet host therefore stamps every active type-3
  record's `+0x18` with GameTime each tick** [INFERRED: in a network game `+0x18` is the time last
  heard from that player]. MEASURED: seats flipped to 3 in a live skirmish with `+0x18` = 0 ran
  3 ticks in about 8 s (GameTime 9596 → 9599); with the stamp, 30.0 ticks/s.
- **The elimination message `0x47BD70(player)`** (stdcall, `ret 4`) formats one of three texts
  (`0x507B88`, chosen by `0x4E4870() % 3`) with the player's side (`0x5072D8` "Core" or
  `0x5072E0`) and sends it through `0x463CA0(buf, 4, 0, player+0x146)`. The destructor decrements
  the owner's live count `+0x144` (`0x486DFC`) and at zero calls, by game type `0x435100`,
  `0x450380(owner+4)` in a network game (`0x486E32`) or `0x47BD70(owner)` in a skirmish
  (`0x486E54`). A rebuild's clear takes every seat through zero, so each would announce an
  elimination. MEASURED: the probe retargeted `0x486E54` to a filter that drops the call while
  its own rebuild runs, a flag of the game thread: 20 of 20 suppressed over the rewind runs, none
  let through.
- **The movement pass's parts.** `0x43DD20` (*The move object `0x43DC00`*, below) calls the class
  step `[vt+0x08]`, then `0x43D290` (def `+0x241` bit 11) or `0x43CD20`, then `0x43D6D0`,
  `0x43DA70` and `0x43DB50`, each thiscall on the move object with the unit as its argument,
  `ret 4`:
  - **`0x43CD20`**, the ground step, asks the class `[vt+0x14]` whether it has a course. The remote
    ground proxy's is `0x44F5B0`, which answers proxy `+0x18` ≥ 2, the fed path count. Answered 0,
    it stores 0 at the move object's `+0x24` and calls `0x43CC20(unit, −def+0x19A)`
    (`0x43CD31..0x43CD52`), which adds that to the speed `+0x20`, floored at 0
    (`0x43CC2D..0x43CC3B`) [the rest of `0x43CC20` not read]. **An unfed proxy therefore brakes
    every step.**
  - **`0x43D290`**, the air step: unless the move object's layer `+0x2E & 3` is 2 it zeroes the
    velocity `+0x8..+0x10`, the speed `+0x20` and `+0x24` and returns (`0x43D297..0x43D2BD`); on
    layer 2 it asks `[vt+0x10]` (`0x43D2EE`) and steps the velocity (`0x43D2C0..0x43D6BF`) and then
    the smoothed acceleration and the bank through `0x43D0D0` (`0x43D6B3`): the parts the demo
    recorder's unit frame predicts are in *The demo recorder's unit frame* (below); the steering
    term between them is the size research's reading.
  - **`0x43D6D0`**, the placement. A carried unit (`+0x86` set) is put at its carrier's attach
    point (`0x43E060` by `+0xF9`; for def `+0x241` bit 19 not below the sea level `main+0x1427F`
    less the waterline def `+0x22C`, `0x43D712..0x43D75A`) through `0x48A9F0(unit, pos, move
    object +0x2E & 3)` (`0x43D775`), turned from `0x43E180` (`0x43D78E..0x43D7A1`), given its
    carrier's speed and velocity or zeroes (`0x43D7AB..0x43D7E8`), and `+0x110` bit 16 is cleared
    (`0x43D7EB`). Otherwise the new position is the position plus the velocity and **the layer is
    the move object's** `+0x2E & 3` (`0x43D7FF..0x43D839`); unchanged position and layer do
    nothing (`0x43D83D..0x43D854`), else it stamps `+0x2A` with GameTime (`0x43D85A..0x43D865`),
    moves the unit and writes `+0x2E` bit 2 = (`0x47DB70` answered 0) (`0x43D912..0x43D92C`)
    [INFERRED: the footprint could not take the cells; the rest not read].
  - **`0x43DA70`**, the walk scripts: the class is 0 while the move object's `+0x2E` bit 2 is set,
    while the unit is carried, or while the speed `+0x20` and the word `+0x24` are both 0; else 1
    up to def `+0x1AE`, 2 up to def `+0x1B2`, 3 above (`0x43DA70..0x43DABD`). A class different
    from `+0x110` bits 2–3 starts `StopMoving` (class 0), or `StartMoving` when bits 2–3 were 0,
    then `MoveRate1`–`MoveRate3` by class (`0x43DAD1..0x43DB27`), and stores it
    (`0x43DB2C..0x43DB3D`).
  - **`0x43DB50`**, the water state: 0 off layers 1 and 2, otherwise 1–4 from the altitude `+0x70`
    against the sea level, def `+0x22C` and def `+0x170` (`0x43DB65..0x43DBC7`), and the old value
    when none of the tests holds, so it lags the altitude; a change starts `setSFXoccupy` with it
    (`0x43DBE8`) and stores it at `+0x10A`.
- **The move object's layer and bit 2 have their own writers.** `0x43D210` (thiscall on the move
  object (unit, layer), `ret 8`) does nothing for the layer it already has; for layer 1 it zeroes
  the velocity and speed, calls `0x43D0D0` and clears state-byte bit 0 through
  `0x48B090(unit, 1, 0)`, for any other layer sets it (`0x48B090(unit, 1, 1)`, starting
  `Activate`); then it stores the layer in `+0x2E` bits 0–1 (`0x43D210..0x43D28C`). Its callers
  are nineteen sites in order handlers between `0x40F23F` and `0x415863` and the remote air
  proxy's reader `0x490A10` (`0x490A8C`). The constructor `0x43DC00` sets layer 1 and clears bits
  1–2 (`0x43DC25..0x43DC2F`), the detach writes the layer bits (`0x48ACE3..0x48ACF0`), and
  `0x43DD70` restores the byte from a copy (`0x43DEC8..0x43DEDA`). MEASURED: a replayed aircraft whose
  `+0x110` layer was written 2 read layer 1 after the pass and sat on the ground, 74–212 wu below
  the recording; with the move object's layer written too, it flew within 2.5 wu.
- **The altitude fix-up `0x48A870(unit)`** runs when `+0x110` bit 16 is set or def `+0x241` has
  `0x1000`, and clears bit 16; it acts only on a unit with a mover on layer 1. With def `+0x241`
  bit 20 the altitude becomes, with bit 12 also set, the larger of the terrain height
  (`0x485070(pos)`) and the sea level less the waterline def `+0x22C`, else the terrain height;
  with bit 19, the sea level less the waterline; otherwise `0x48A490(unit)`, the fit to the
  footprint's corners with the hover wave on the wall clock (*The unit and player records, byte by
  byte*, below).
- **The round robin's position write** (`0x48B5CA..0x48B6F9`, in `0x48B3F0`): x, altitude and y
  read as 32-bit values and three 16-bit turns (`0x415DC0`); the footprint cell from `+0x7E`/`+0x80`
  (`0x48B619..0x48B648`). The same cell and layer write the position alone
  (`0x48B64B..0x48B682`); otherwise `0x47D0E0(unit)` releases the cells, the position, the cell
  `+0x76` and the layer in `+0x110` bits 0–1 are written, `0x47CC30(unit)` claims and `0x4827B0(unit)`
  stamps sight (`0x48B684..0x48B6BF`). Then `+0x110` bit 16 is set, the turns written to
  `+0x64..+0x69` and the move object's `+0x20` (`0x48B6C4..0x48B6F9`). The probe's player wrote
  positions by this recipe.
- **MobileBuild `0x403A20`** (stdcall (unit, node, events), `ret 0xC`): event bit 3 →
  `0x47F780(unit, 7)` and `0x41C110`, answer 8; bit 1 → `0x41C110`, answer 5; otherwise by the
  state `node+5` through `0x403F5C` = {`0x403A8D`, `0x403B45`, `0x403DF5`, `0x403E0C`,
  `0x403F2D`}. State 0 snaps the site to the footprint (def `+0x14A`) into `+0x22`/`+0x2A`, clears
  `+0x3E`, calls `0x438AD0`, sets node `+6` = `0xE0` and answers 1; state 2 calls
  `0x438700(unit, node, 10)`; state 3 (`0x403E0C..0x403F2A`) builds the frame at `node+0x16` at
  def `+0x1FE` / 30 per call through `0x41BA60(builder, node+0x16, rate)`, sprays when that
  answers nonzero (`0x43E400`, `0x4720D0` to the frame's corners from def `+0x15E`/`+0x16A`),
  stamps the builder's `+0xB0` = GameTime + 300, and answers 1 while the frame's `+0x104` is 0,
  else re-arms with `0x439E80(node, 1)`, sets node `+6` bits `0xA` and answers 2; state 4 calls
  `0x47F780(unit, 8, 0x501530)` and answers 5. `0x439E80(n)` (thiscall on a node, `ret 4`) sets
  node `+6` bit 0 and node `+0xA` = GameTime + n.
- **Adopting a frame.** A re-created BuildingBuild or MobileBuild node gets no target, as the order
  add stores none. The probe set the node's reference at `+0x12` to the recorded frame through
  `0x489690` (the reference set, *The unit references inside a `0x2C`*, below; its unit is the
  node's target `+0x16`) and the state `node+5` to 3. MEASURED: the factory and the builder went on
  building the frames they had, and without adoption the frame was destroyed within 13 s.
- **A node's owned object `+0x52`** is deleted by the order destructor through its vtable slot 0,
  `call [[+0x52]](../1)`, and the field cleared (`0x43A288..0x43A2A4`). MEASURED: a rebuild that copied
  `+0x52` from a snapshot deleted a stale object and faulted at `0x43A2A2`; a rebuild writes
  recorded values into fresh nodes and never an engine pointer.
- **Action bytes met** (an order's type byte `+4` is its rank in the name-sorted descriptor vector,
  *`0x512344` — the order-descriptor array*, below): 6 Attack_Chase (`0x4034A0`), 11 BeCarried
  (`0x402FC0`), 12 BuildingBuild (`0x402640`), 19 GetBuilt (`0x402DA0`), 25 MobileBuild
  (`0x403A20`), 26 Move_Ground (`0x4031D0`), 30 QMove (`0x403160`), 34 RepairPatrol (`0x405980`),
  41 Standby (`0x405FE0`), 46 Suppress (`0x4038A0`), 55 VTOL_Move (`0x40FA20`), 56 VTOL_Patrol
  (`0x410E70`). A lab's rally is a QMove on its main list.
- **The event senders test the network bit.** 37 sites test `main+0x2A44` bit 0 (`test byte
  [reg+0x2a44], 1`). In a skirmish the probe saw only Impure's own wrapped deaths and creates
  (`0x05`) and `0x0A`, `0x11` and `0x12` sent: the `0x11` sender in `0x48B090` tests the owner's type (1 or 2,
  `0x48B1B5..0x48B1F3`), not the bit. Releasing eleven of the tests — seven in the weapon code
  (near `je` at `0x49D7A7`, `0x49DA99`, `0x49DC6B`, `0x49DE2C`, `0x499ABF`; short `je` at
  `0x49DFB6`, `0x499BAF`) and the four `0x10` senders' (short `jne` at `0x45619F`, `0x456225`,
  `0x45629F`, and `0x456135` in `0x456110`, a fourth sender with no E8 caller that starts a script
  by name through `0x4B07C0` with no arguments) — made a skirmish send its shots `0x0D`, script
  starts `0x10` and `0x0F` (MEASURED: 1,731, 5,751 and 3 in a 60 s battle of 1,432 units). The
  `0x0E` send at `0x49A7A8` was not found behind such a test [its function not traced to the
  start], and no `0x0E` was seen.
- **What the replayed messages carry.** `0x0D` names its target at `msg+0x1F` and its shooter at
  `msg+0x21` (the sender writes them from `+0xA8` at `0x49D802..0x49D81D`); `0x0C` its victim at
  `+1` and killer at `+7`, `0x10` its unit at `+1` (*The network receivers take a unit index
  straight off the wire*, below, for the receivers' own bounds); `0x0F` carries a subcode at
  `+1`: `0xFD`, `0xFE`, `0xFF` → `0x4244B0`, `0x423550`, `0x4233A0` (case `0x45544D`).
  MEASURED: fed through `0x4866D0(msg, 0)`, `0x49D270(0, msg)` and `0x4B0B00` with every index
  bounded by the player, a battle's 84–130 deaths, 1,566–1,731 shots and 5,751–6,019 script starts
  ran with none refused and no fault, and the live set matched the recording at every checkpoint.
- **The grid also writes `+0x8E` and `+0x82`** (`0x47CB26`, `0x47CBB0`, `0x47CBB9`, `0x47CC13`,
  `0x47CD1C`, `0x47CD25` among others), beside the attach's next passenger `+0x8E` (`0x48AC7C`,
  cleared at `0x48ACBC`).

### The demo recorder's capture: where it runs and what it reads [DISASSEMBLED + MEASURED 2026-09-29]

S1 of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s1-the-recorder)
(`tagpu_recorder.c`) reads the engine and writes none of it. It runs first in B2's stub on the sim
step `0x495490`'s call of the unit tick (`0x4954ED`,
[*Stacked aircraft in area damage*](weapons.md#stacked-aircraft-in-area-damage-0x49a664-0x49a415-0x47cf98-0x4954ed-disassembled-measured-2026-09-25)),
after the step's GameTime increment (`0x4954C0`), so it sees the world between two iterations. The
step's network pump (`0x453D40` at `0x4954C8`) runs only for a nonzero argument (`test edi,edi` at
`0x4954BE`, `je 0x4954ED` at `0x4954C6`): the frame function `0x496790` passes 1 in a network game
(`main+0x2A44` bit 0, tested at `0x4967E7`; `push ebx` at `0x49680B`) and 0 otherwise, a skirmish
among them (`push 0` at `0x49693C`, after the pause test at `0x496920`), so no pump runs before a
skirmish capture. Native mode-3 capture follows that pump at the same B2 call.
The file context attaches before any contribution publication/consumption; its
primary stream contains the recorder's observed world. Native `cap_orders` and
`cap_verify` bound each owner pointer to ten player records, then skip controller
3/4 lists. Exact owner queues remain in the separate ledger instead of being
attached to a lower-bound/unknown replica birth. No engine byte patch or field
write is added by file integration.

| What | Where | The bound, and what else is established |
|---|---|---|
| the game mode | `[main+0x391E9]+0`, what `0x435100` (`mov eax,[ecx]`) returns | 2 in a skirmish, and in a skirmish loaded from a save: MEASURED, a skirmish saved at GameTime 1846 and loaded from the in-game menu read 2, and its first iteration was 1847; native file capture requires mode 3 plus `main+0x2A44` bit 0 and a validated contribution channel. The four-actor native file exercises that guard at GameTimes 1–536 |
| GameTime | `main+0x38A47` | a recording ends if it goes back without a teardown |
| the unit array | begin `main+0x14357`, its last slot `main+0x1435B` (inclusive), `u16` slots `main+0x14351` | used only when last = begin + (slots − 1)·`0x118`; it lives from the level load `0x4854A0` to the teardown's first free, `0x485980` called at `0x491B95`, and the recording ends on entry to the teardown |
| a unit's liveness and type | `+0x110` bit 28; `+0x92` | a slot whose type changes is recorded whole |
| the unit types | `*(main+0x1439B)`, `main+0x1438F` of them, stride `0x249`, UnitName `+0x20` (`0x20` bytes) | the header's fingerprint, and the bound of a type ID |
| units each | `u16 main+0x37EE6` | the header |
| the players | ten records from `main+0x1B63`, stride `0x14B`: `+0x27` PlayerInfo (`+0x95` side, `+0x96` logo), `+0x2B` the name (`0x1E` bytes), `+0x73` the controller | PlayerInfo lives as long as the process: the constructor `0x463BE0` allocates it for main's eleven records (the loop at `0x41D98B`), the reset keeps it (`0x464508`), and the destructor `0x463C40` runs only on a stack-local record (`0x454538`, `0x454607`) — [*The per-map arrays*](memory.md#the-per-map-arrays-who-allocates-who-frees-and-in-what-order-mapped-by-us) |
| the sort buckets | `*(main+0x1429F)`, `main+0x142A3` columns × `main+0x142A7` rows (LoadMap, `0x482C84..0x482CA6`), and the off-map bucket `*(main+0x142B7)` | an id row's table; one past the last bucket is an index (stock forms it at the east edge), and the off-map bucket has an ID of its own. `0x412834` compares a unit's `+0x82` with it: `mov edx,[0x511DE8]` (`0x412828`), `mov ecx,[esi+0x82]` (`0x41282E`), `cmp ecx,[edx+0x142B7]`, `jne 0x412849` DISASSEMBLED |
| the map's name | `GameingState +0x204`, the TNT path | the file's name, read to `MAX_PATH` bytes: inside the object, whose fields run past `+0xDBC` (the list `0x423160` walks) |

**A load from the in-game menu runs the teardown before the saved level loads, and in stock the
loaded game starts paused** [MEASURED 2026-09-29; our `fix_load_unpaused` clears the pause
before the first tick, *Engine defects we patch*]: at the load's click the recording in progress ended through
`tagpu_reclaim`'s wrap of `0x491B60`, at GameTime 1846; after the load `main+0x38A51` read 1 and
GameTime stayed 1846 until a pause key, and no iteration ran, so a capture at `0x4954ED` sees
nothing of a paused game.

**The process's exit** [DISASSEMBLED 2026-09-29, and MEASURED the same day], read for the
recorder's exit wait (`tagpu_recorder.h`, *The exit*). The CRT's entry `0x4E6FA0` calls WinMain
`0x49EDA0` at `0x4E70ED` and hands its result to `exit` `0x4E4600` at `0x4E70F6`. `exit` is
`doexit(code, 0, 0)` and `_exit` `0x4E4620` is `doexit(code, 1, 0)` [names INFERRED]. `doexit`
`0x4E4660` first takes the CRT's exit lock, `_lock(13)` (`0x4E4720` calls `0x4EAC60` at
`0x4E4663`), and releases it only on the path that returns to its caller (`0x4E4730` at
`0x4E46F4`, for `0x4E4640` and `0x4E4650`), so the lock is held across `ExitProcess`. For `exit`
it runs the atexit table (`0x52B654`/`0x52B658`) and both terminator tables; for `_exit` the `jne`
at `0x4E469E` skips the atexit table and the first terminator table (`0x50106C`..`0x501074`). Then
it calls `ExitProcess` through the IAT slot `0x4FC100` at `0x4E4708` — or, re-entered once
`_C_Exit_Done` (`0x529F0C`) is 1, `TerminateProcess` (`0x4FC210`) at `0x4E467E`. The game calls
`exit` itself at `0x417AF7`, `0x444381` and `0x4B62B3` (`0x4B6290`, after its message box), and
`_exit` at `0x49E811` (the out-of-memory handler `0x49E700`) and `0x4DA0D1`; the entry's exception
path calls `_exit` at `0x4E7119`. The CRT's fatal paths leave through the same slot: `_amsg_exit`
`0x4E7150` [INFERRED name], 15 callers, calls `[0x50DF10]`, which holds `_exit`, with 255, and
`0x4E7180`, called at `0x4E7002` and `0x4E7015`, calls `ExitProcess(255)` at `0x4E71A0`. Nothing
reaches `0x4EADE0` (`FatalAppExitA`, `0x4FC27C`, then `ExitProcess(255)` at `0x4EADF2`) or the jump
thunk `0x4FB12E`: no call, jump or stored address names either. The in-game
exit to Windows is `0x491C60`: `0x4257A0`, `0x451B60`, the teardown `0x491B60` at `0x491C6A`, then
`0x4B6230(0)` at `0x491C71`, which with a zero argument shows no message box (`MessageBoxA`,
`0x4FC300`, at `0x4B626D` is skipped) and posts `WM_DESTROY` to the game's window
(`PostMessageA`, `0x4FC2D0`, at `0x4B6282`). The exit menu's handler `0x4605C0` (stored at
`0x4607D2`) calls it at `0x460626` when `[0x512FF8]` is 2, after setting bit 2 of
`main+0x3923B`. So the teardown ends a recording before any of the exit runs, and the exe's
`ExitProcess` slot is the one the fork's hook takes. MEASURED: a skirmish left by `EXITGAME` in a
scratch build whose writer held three seconds before finishing: the recording ended at the
teardown, the exit logged its wait for the one file, and the file was complete.

### The demo recorder's event sites [DISASSEMBLED + MEASURED 2026-09-30]

S2a of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s2a-what-watching-needs-recorded)
(`tagpu_recorder.c`, *The events*) records what the engine does in a step that the images after it
cannot say, where the engine does it and in the order it does it; the research is
[the events note](../tadr-port/demo-recorder-s2-events.md). `tagpu_rec_init`, at DllMain's attach,
byte-matches every site and builds every stub before any lands; no recording starts without all of
them (`recorder: 68 event sites in`: thirty-one observers -- the attach `0x48AB70`'s among them
(*The order of a step's footprint and chain calls*) -- five network tests, fifteen named-start
calls, six query calls, six feature draws, the destructor's explosion call, Teleport's
set-position call `0x406BC0`, reproduction's source test and the two click calls; MEASURED in the
replay gate's logs, 2026-10-23). A patch or mod that holds other bytes at any of them --
`0x406BC0` among them -- leaves every event hook out and nothing is recorded; the log names the
site.
Each hook reads only a recording's game thread; a call from another thread is refused and counted
(`RK_END`'s off-thread count). An observer's handler sees the site's `esp` as `E` and the
registers `pushad` saved under it (EDI at `E−0x24` up to EAX at `E−0x08`), so at a function's
entry its first argument is at `E+4`. The events of step T are appended after image T and
published by the next capture, at `0x4954ED`, which ends the recording (`REC_END_RING`) instead if
one did not fit.

| Site | What it is | What the hook reads |
|---|---|---|
| `0x4861B3` (`mov eax,esi; pop edi; pop esi; pop ebp`) | the create `0x485F50`'s success exit; the NULL exits (`0x4861BD`, `0x48605A`) are not observed | `esi`, the unit in the slot it took. Five registers are still pushed (`0x4861B5..0x4861B9`, then `ret 0x20`), so the arguments start at `E+0x18`. The first, the owner, is overwritten at `0x485F9C` (`mov [esp+0x18],eax`, the unit type), so the owner is the unit's own `+0x96`; the type is the word at `E+0x1C` (`mov dx,[esp+0x1C]`, `0x486079`), the position the three dwords at `E+0x20`, `finished` the dword at `E+0x2C` (`0x486075`), and `layer` at `E+0x30`, of which the create keeps bits 0–1 (`0x4860F1`). The events of the calls the create makes (its state byte through `0x48B090(1, 1)`, its scripts) come before the create's own |
| `0x486075` (9 bytes: `mov edi,[esp+0x2C]; mov dx,[esp+0x1C]`) | multiplayer constructor-begin observer, after the selected slot and its B4 birth token are fixed, before type/init/COB work | Same five saved registers and arguments as the success exit; `esi` is the actual selected fixed-array slot, including a first-free slot B4 moved. Copies the same 28-byte arguments/water image into contribution-only root `0x100`. Neither stolen instruction is relative. Ordinary skirmish events remain success-exit observations. |
| the destructor's entry (9 bytes) | [`0x4866D0`](memory.md#0x4866d0-the-unit-destructor-inferred-name-units_destroy) `(rec, mode)` | the record: the victim's `u16` at `+1`, the killer's netid at `+3`, the killer's `u16` at `+7`, the severity at `+9`, the kind at `+0xA`. A call the destructor would make inert (B3's bound at `0x4866E5`, the alive test at `0x486706`) is not recorded; a killer of 0 is none (`0x48675A`). The killer's player by `0x44FE40`'s rule (below); C4's carried decision (`tagpu_tx_carried`); the victim's image as the destructor finds it |
| `0x41B8D0` (6 bytes) | the build done `(builder, built)`, `ret 8` | both arguments. Its five callers are listed at the table's `0x41B8D0` row above; the build step `0x41BA60` calls it at `0x41BCBF` when the built unit's `+0x104` compares equal to 0.0 (`0x41BCAA..0x41BCBB`), so every build that finishes in play passes here |
| `0x48B090` (5 bytes) | the state setter `(mask, value)`, `ecx` the unit, `ret 8` | the byte `+0x10E` as the setter will leave it (`0x48B09A..0x48B0D4`), recorded only when it changes, since the setter returns at once otherwise (`0x48B0DE`) |
| `0x47DAF1` (6 bytes: `mov [esi+0x10F],bl`) | YARD_OPEN `0x47DAC0`'s write, past its refusal `0x47D970`; no branch targets `0x47DAF2..0x47DAF6` | `esi` the structure, `bl` the new `+0x10F`: its bit 2 as `RK_EV_YARD` (*An open yard gives nothing back*), held until its create when the structure is inside it, kept out where THE YARD RULE would refuse it; skirmish recordings only |
| `0x456190`, `0x456290` (5 bytes each) | the broadcast starts `(unit, idx)` and `(unit, idx, argc, a0..a3)` | the arguments as the `0x10` carries them |
| `0x456218` (6 bytes) | inside the broadcast start by name `0x456200(unit, name, argc, a0..a3)`, after `0x4B07C0` has turned the name into the index (`0x456213`) | `esi` the unit (`0x456208`), `eax` the index, the arguments at `E+0x28` (the function's `sub esp,0x18; push esi`) |
| `0x47F780` (6 bytes), `0x47F850` (5 bytes) | the unit replies `(unit, kind, text)`, before their viewer tests (`0x47F850` tests first that the unit is off screen, `0x48BCB0` at `0x47F856`) | every player's reply, the text copied only to its end. `0x47F7E0`, between them, is never called: no call, jump or stored address names it, and its address occurs nowhere in the image |
| `0x43E400` (8 bytes) | `QueryNanoPiece` `(unit, out)` | the unit and the out pointer, for the spray's credit (below); its query as `RK_EV_QUERY` (*The owner's COB queries*) |
| `0x4720D0`, `0x472200` (7 bytes each) | the nano emitters `(point, box, layer)` and `(box, point, layer)` | the point, the box and the layer, credited to a unit |
| `0x43A216`, `0x43B878`, `0x43BB1D` (8 bytes each) | the order dispatch: the order's `mov`, its `push`, and the `call [reg+4]` 4 bytes on (`0x43A21A`, `0x43B87C`, `0x43BB21`), the only ways into a handler ([*The nano spray*](weapons.md#the-nano-spray-two-emitters-fifteen-order-handlers-disassembled-2026-09-29)) | nothing: they forget the last query |

**Owned constructor contributions.** The existing `0x4861B3` observer also
copies successful local human/hosted-AI constructor arguments into kind-10
contributions with recording off. Fixed unit/player array bounds and the owning
game thread protect the reads; the frozen origin block and source-roster owner
row authorize the copied payload. B4 stamps the slot before constructor calls,
so the queue attaches its exact incarnation token even when replies precede
this success exit. The payload retains all 32 finished bits: `0x486075` loads
the dword and `0x48611A..0x48611C` test zero versus nonzero, without normalizing
it. The position copies at `0x48607E..0x48609D`, raw type store at `0x486086`,
layer mask at `0x4860FE` and success register pops at `0x4861B3..0x4861B9`
were checked against pristine disassembly. This is the successful return;
constructor child roots can precede it.

**Constructor reservation before child roots [DISASSEMBLED 2026-10-05].**
`0x486075` now supplies a distinct contribution-only begin root (`0x100`), with
the same 28-byte argument shape. B4's `hit_take` at `0x486036` runs after the
free-type test, selects the actual slot and assigns its exact monotonic stamp
before continuing at `0x48605D`. Every refusal/null return in `0x485F50` is
before the observed boundary; the subsequent body has one successful return.
The pool initializer assigns each slot's fixed owner pointer at `0x485902`
and owner-row byte at `0x48590E`, within the ten player blocks ending at
`0x48592B`. Thus this observer need not follow a COB, definition or mover from
a not-yet-initialized slot: unit/player array arithmetic, frozen owner/block
agreement and game-thread level lifetime protect its owner/water reads. The
source tick and stamp are copied by the same existing `mp_roots_prepare` path.

The stolen span is exactly `8B 7C 24 2C 66 8B 54 24 1C`. There is no branch
into its second instruction in the constructor. A later candidate at
`0x4860A5` would steal `push esi; call 0x485D40`: the generic observer copies
stolen instructions without relocating a relative call, so that span cannot
be used with it. Attach matches the whole chosen span along with all other
observer sites before installing any. This adds one observation byte patch
and no engine field write. The game-thread queue preserves begin, child roots
and successful return in source call order; no look-ahead or retiming is needed
to discover the new incarnation. Engine materialization remains open.

**Constructor initialization calls read for that ordering [DISASSEMBLED].**
Type is stored at unit `+0xA6` at `0x486086`. `0x4860A0` calls
`0x485A40(unit, position-by-value, finished)`, `ret 0x14`: it sets the
definition pointer `+0x92` and live bit (`0x485A75`/`0x485A70..0x485A7B`),
unfinished fraction/HP `+0x104`/`+0x108` (`0x485B11..0x485B37`) and position
`+0x6A`/`+0x6E`/`+0x72` (`0x485B77..0x485B96`). It calls the simulation RNG
`0x4B6C30` at `0x485BDE` and `0x485D1B`, weapon helpers `0x48A160` and
`0x489800` at `0x485C50`/`0x485C58`, inline resource-object constructor
`0x401070` at `0x485D0A` and group-add `0x480250` at `0x485D29`. The
begin observer precedes all these calls; their transitive effects are not
claimed absent from this read of the initializer.

`0x4860A6` then calls `0x485D40(unit)`, which allocates `0x544` bytes for COB
and stores unit `+0x9A` at `0x485D9F`, loads the script via `0x4B0720`
at `0x485DAC`, makes the model via `0x45A950` at `0x485DC0`, stores
`+0x9E` at `0x485DCC` and binds it via `0x480D40` at `0x485DD2`.
Its `Create` script call at `0x485DE6` is `0x4B0940(name, 0, 1)`:
`0x485DDD` pushes run-now 1, before the outer constructor returns. The
no-script path calls `0x45A8D0` at `0x485E09` and leaves COB null. The
outer constructor subsequently calls weapon init `0x49E070` at `0x4860AC`,
order init `0x437840` at `0x4860B2`, placement `0x48A870` at `0x486109`,
footprint `0x47CC30` at `0x48610F`, network create `0x456050` at
`0x486115` and group/update helpers `0x482AC0`/`0x490580` at
`0x486178`/`0x4861AE`. This establishes why an exit-only creation root is
too late for child calls, rather than asserting that initialization is inert.

MEASURED 2026-10-05 on the reference setup: the four-actor native coverage
fixture records 71 begin/success pairs, each with the same exact payload, birth,
source tick and phase. Eight kind-3 reply calls lie between their own begin and
success. All 28 first-kind observations match publisher/recipient and persisted
batch bytes/provenance, including the peer with saving off. The complete file
has 558 iterations, zero corrections and 8,146 exact application envelopes
(2,061,238 bytes, CRC `8EA54D8E`) retained through C/Python reader world seek.
Both peers reach the new boundary/payload helpers in the retained coverage map.
This is constructor capture/order evidence, not native engine playback.

**Owned destructor contributions [DISASSEMBLED, MEASURED 2026-10-05].** The
existing `0x4866D0` observer now also brackets deaths of bounded frozen local
human/hosted-AI slots with recording off. Its 340-byte entry payload copies the
victim/mover image before orders are freed (`0x486D75`), COB is deleted/nulled
(`0x486D8A`/`0x486D8D`), model state is freed/nulled (`0x486D9E`/`0x486DA3`),
and the mover is destroyed/freed/nulled (`0x486DB1`/`0x486DB7`/`0x486DBF`).
The slot type is then cleared at `0x486DC7` and live bit at
`0x486DCE..0x486DD4`; the return epilogue starts at `0x486E59`, `ret 8` at
`0x486E60`. The return observer publishes only its saved origin, source tick,
capture phase, unit ID and exact birth, so it follows no cleared slot or freed
object. Slot/player bounds and the owning game thread establish the entry's
destructor lifetime; a pointer probe is unnecessary.

**Lifecycle children and the engine consumer [SOURCE, DISASSEMBLED, MEASURED
2026-10-05].** The saved constructor-boundary fixture contains five
`TargetCleared` starts on their own dying unit between entry and return, in
addition to its eight constructor replies. The copied lifetime consumer retains
the target through each bracket and normalizes the entry's killer row and
victim image; its return action carries that saved image. C/Python dispatch
retains 71 constructor and 24 death pairs, all 6,000 owned slots and exact action
CRC `8CF737F2` through fresh reader seek. Native application remains open.

The existing skirmish `make` in `tagpu_replay.c` opens both COB-write and reply
gates around `0x485F50`; `tagpu_puppet.c`'s `s_open` is tested by both predicates.
That reproduces children which precede the skirmish's exit-only create and have
no live target when replayed. Multiplayer's begin/child/success sequence cannot
reuse that rule while also dispatching the recorded children: it needs an
explicit sole producer for those effects. The observer stubs for the fifteen
owner-only named starts restore registers/flags and jump unconditionally to
`0x4B0A70` or `0x4B0940`; their C observer's return value suppresses nothing.

DISASSEMBLED against `pristine/TotalA.exe.pristine`: the destructor calls order
purge `0x439EB0` at `0x4867A1`, notice cleanup `0x47F8C0` at `0x4867A7`, group
removal `0x480250` at `0x4867AF`, projectile source cleanup `0x49C880` at
`0x4867B5`, cargo detaches `0x48AAC0` at `0x4867CB`/`0x48681D`, cargo hit
`0x489BB0` at `0x48680B`, footprint removal `0x47CBD0` at `0x48682D` and
optional sight removal `0x482090` at `0x486845`. Its explosion `0x49B000` at
`0x486D50` and corpse helper `0x486360` at `0x486D6F` precede owned-object
destruction. None of these observations proves transitive child effects absent.

The initializer's three `0x489800(k)` calls at `0x485C58` cannot start a script:
each follows `0x48A160(unit, k)` at `0x485C50`, which writes slot `k`'s target
words as `0`/`0x8000` (`0x48A173`/`0x48A178`), the pair `0x489850..0x489865`
tests before skipping the start; `k` stays below 3, so there is no recursion
[DISASSEMBLED 2026-10-05]. Direct-call reachability over the retail executable
(functions are call targets plus table-referenced starts) from `0x485F50` reaches
this start site and the reply `0x47F780` (through the state setter at
`0x48B110`). From `0x4866D0` it reaches HitByWeapon/TakeDamage and the reply
`0x47F850` through the cargo hit (`0x48680B` → `0x489BB0` → `0x489CE0`),
and `0x489800`'s start through the order purge (`0x4867A1` → `0x439EB0` →
`0x43A1F0` at `0x43A2B9`). No other owner-only site is directly reachable;
118 and 131 indirect calls in those call graphs are not followed.

The reset `0x489800` has no controller test before its `TargetCleared` start:
selector 3 recursively resets selectors 0 and 1 at `0x48980E`/`0x489817`, then
falls through with 2. Weapon-row `+0x1B` bit 1 set/bit 4 clear
(`0x489838..0x489842`) and nondefault target words (`0x489850..0x489865`) cause
bit 4 to be set (`0x489844..0x489846`), target words to become
`0`/`0x8000` (`0x489867..0x48986C`), helper `0x4B07C0` at `0x48987B`, then
named start `0x4B0A70` at `0x489898`. A type-3 owner does not itself prevent this.
`0x49C880` scans the projectile pool at `main+0x141F7` for the count
`main+0x141F3` (`0x49C888..0x49C90A`). A nonzero projectile `+0x60` and
`+0x52` equal to the dying unit select a row (`0x49C8A1..0x49C8AA`);
it cleans the current-shot fields when selected (`0x49C8AC..0x49C8EA`), sets
`+0x69` bit 1 (`0x49C8F0`) and calls `0x49AE20` at `0x49C8F4`. The read body
does not directly call `0x489800`; that is not a claim about its callees.

The current four-actor native coverage capture has 22 matched entry/return
pairs, including 12/5 deaths from the non-recording peer's human/hosted AI.
All five locally owned primary death payloads equal their mandatory copied
payloads byte for byte at the same source tick. Both peers reach entry, frame
allocation/return and copied publication in the matching retained coverage map.
The complete file still carries its separate observed-world events: it has
23 primary deaths, so the two namespaces are not interchangeable. These are
capture facts, not native replay application or complete death-effect authority.

**`0x44FE40(netid)`**, `ret 4`, read and not hooked: −1 is player 10, none (`0x44FE47`,
`0x44FE9B`); otherwise the first of the ten seats whose controller `+0x73` is nonzero and whose
`+4` is the netid (`0x44FE5C..0x44FE99`), and 10 when no seat matches. The death's killer is named
by the same rule.

**Copied reply bounds [DISASSEMBLED 2026-10-05].** `0x47F780` loads the unit
at `0x47F787` and compares unit owner byte `+0xFF` with viewed row
`main+0x2A43` at `0x47F78B..0x47F799`, before reading the caller's text.
It tests live bit 28 and carried bit 14 at `0x47F79B..0x47F7AB`. With null
text, `0x47F7BA..0x47F7BD` compute `kind*24` and read table field `+0x10`
at `0x5086E8`, without bounding kind; translation `0x4C5740` is called at
`0x47F7C5`, notice queue `0x47FAD0` at `0x47F7D3`, then `ret 0xC`.
The off-screen root `0x47F850` calls visibility `0x48BCB0` at `0x47F856`,
returns for visible units, tests the same viewed-owner/live/carried fields,
and performs the same null-text lookup at `0x47F893..0x47F896`, translation
at `0x47F89E`, and notice call at `0x47F8AC`; it returns twelve bytes at
`0x47F8B3`. The intervening uncalled helper `0x47F7E0` has the same
null-text lookup/translation/notice at `0x47F826`, `0x47F82E`, `0x47F83C`;
no new observer is added there.

The notice table is **24 rows of 24 bytes**, `0x5086D8..0x508917`:
`.data` contains row indices 0 through 23, and the next string begins at
`0x508918`. `0x47FAD0` scales kind by 24 at `0x47FADE..0x47FAE9`, reads
GameTime at `0x47FAE3`, and compares field `+0x14` at `0x47FAF0` without
bounding kind. Its queue holds eight 17-byte entries (count test
`0x47FB1A`); it calls queue helper `0x47FD70` at `0x47FB27`. Priority
lookups at `0x47FBA4` and `0x47FBB1` use row field `+4` (`0x5086DC`),
with the latter scaling a queued kind by 24 at `0x47FBAE`. The table's
stride was previously documented as eight; that claim is corrected here.
Copied replies require kind <24, boolean root/text flags, zero reserved
bytes and at most 128 non-NUL text bytes; absent text has no text payload.
The multiplayer owner check bounds the unit and player arrays and filters
replica controllers before copying caller text. Existing observers precede
the viewer filters so hosted AI replies can be retained for their own POV.
These are capture/decoder facts, not engine replay application coverage.

**The nano credit.** Each of the fifteen handlers that spray asks `0x43E400` for the point first,
into a buffer on its own stack, and passes that buffer to the emitter as the point. A spray is
credited to the unit of the last query whose out pointer is its point, and the dispatch sites
forget the last query, so a query cannot credit another call's spray. A spray with no such query is
recorded with ID 0 and counted.

**The script starts only the owner's machine makes** — `call`s of `0x4B0A70(name, p2, now, argc,
a0..a3)` or `0x4B0940(name, p2, now)`, thiscall on the unit's COB object, which each site loads
with `mov ecx,[reg+0x9A]` just before. `0x4B0A70` turns the name into the index and calls
`0x4B0B00(idx, p2, now, …)` (`0x4B0AEE`): `p2` is an object whose first method it calls with 0 when
the script is missing (`0x4B0B11..0x4B0B1D`), and 0 at every site below; `now` nonzero runs the
script at once (`0x4B0B7E..0x4B0B84`). Each `call` is redirected to a stub that records the start,
after checking that the unit's `+0x9A` is the `ecx` loaded (a mismatch is counted as a bad ID), and
then jumps to the function as the call would:

| Script | The `call` (the `mov`), the unit's register |
|---|---|
| HitByWeapon, TakeDamage (the hit `0x489CE0`) | `0x489F43` (`0x489F2C`) `esi`; `0x489F8E` (`0x489F76`) `esi` |
| TransportPickup, TransportDrop | `0x406834` (`0x406823`) `edi`; `0x4069BF` (`0x4069A6`) `edi` |
| TargetCleared, in `0x489800`, `0x4898B0`, `0x48A0F0` and `0x48A1E0` | `0x489898` (`0x489880`), `0x489948` (`0x489930`), `0x48A149` (`0x48A131`), `0x48A2E0` (`0x48A2C8`), all `edi` |
| EndTransport, through `0x4B0940` | `0x40F433` (`0x40F424`) `ebp`; `0x41148D` (`0x411480`) `esi`; `0x411794` (`0x411785`) `ebx`; `0x411DA1` (`0x411D92`) `ebx`; `0x411E2B` (`0x411E1E`) `ebp` |
| BeginTransport (VTOL_Pickup's state 3) | `0x4113EF` (`0x4113DD`) `esi` |
| setSFXoccupy (the water state `0x43DB50`) | `0x43DBE8` (`0x43DBD0`) `esi` |

DISASSEMBLED 2026-10-05 against `pristine/TotalA.exe.pristine`: the pickup argument at
`0x406815` is the zero-extended `u16` unit slot at `[eax+0xA8]`, pushed at `0x406822`;
`0x406823` loads the transport's COB from `[edi+0x9A]`, and `0x406829`/`0x40682B`
push `argc=1`/`now=1` before the `0x406834` call. Drop likewise zero-extends
`[edx+0xA8]` at `0x4069AC` and pushes it at `0x4069B3`, followed by `argc=1`
and `now=1` at `0x4069B4`/`0x4069B6` before `0x4069BF`. Its caller also pushes
a value formed at `0x406995..0x4069A3` into the unused second argument slot;
that value's meaning is not established here. The existing named-event payload
retains all four argument words, including unused ones. The named transport's
first argument is a global unit slot, not a heap address.

**Start ABI and caller continuations** [DISASSEMBLED 2026-10-05].
`0x4B0940` saves ECX and the four nonvolatile registers at `0x4B0940..0x4B0944`;
both its failure exit `0x4B09A8..0x4B09AF` and success exit
`0x4B09ED..0x4B09F7` restore them and use `ret 0xC`, returning 0 or 1.
The by-name argument entry `0x4B0A70` saves the same registers and forwards all
eight words to `0x4B0B00` at `0x4B0AEE`; `0x4B0AF3..0x4B0AF8` restores them
and uses `ret 0x20`. Suppressing a call therefore requires the entry's actual
callee stack cleanup, not merely declining to record it.

All fifteen owner-only sites pass a null callback. The four `TargetCleared`
sites pass `argc=1`, with the selected weapon index as argument 0, and `now=0`.
`EndTransport` runs now at `0x40F433` and `0x411DA1`; the other three sites pass
0. Pickup and drop run now and pass `argc=1`. BeginTransport runs now with
`argc=1` and the passenger def's `+0x16E` (`0x4113D7`) as argument 0;
setSFXoccupy runs now with `argc=1` and the new water state (edi) as argument 0.
These values are carried by the named-root payload; they are not
interchangeable scheduling choices. [DISASSEMBLED 2026-10-13: BeginTransport
and setSFXoccupy.]

After the call, `0x489F48`, `0x411492` and `0x411799` overwrite EAX before
using it, `0x4113F4` pushes constants and ebx for the broadcast, and `0x43DBED`
stores edi; `0x48A2E5` explicitly zeroes it. Pickup calls reply at `0x40683E`,
drop calls the order helper at `0x4069CF`, `0x40F43A` allocates an order,
`0x411DA9` calls an order helper and `0x411E33` loads another value before any
local use of the script result. The tails at `0x489F93`, `0x48989D`,
`0x48994D` and `0x48A14E` instead return with that EAX still present;
their outer callers' use is not established by this continuation audit.
No continuation read here branches on the start's flags before another
instruction establishes flags. Native child suppression is not implemented.

The multiplayer adapter reuses these fifteen call observers and the extra-weapons
helper without adding a byte patch. On the game thread it bounds the observed unit
against the level's fixed array before reading `+0x96` or `+0x9A`, confirms the
COB equals the intercepted `ecx`, and contributes only frozen locally controlled
human/hosted-AI blocks. It copies the existing named-event bytes plus the exact
incarnation token and whether the call preceded B2. `0x4954C0` advances GameTime
before pump `0x4954C8` and capture call `0x4954ED`: a pump root can therefore be at
T while the channel still owns T−1. Copied post-capture T−1 roots drain before
that tick is sealed; copied pre-capture T roots drain after channel advance to T.
The queue's fixed bound and same-thread teardown ordering establish its lifetime;
no engine object follows it onto the worker. `hit_next_birth` may advance a slot's
token past GameTime after repeated same-tick creates, so the token's high bit and
exact identity are checked without treating it as an upper-bounded timestamp.

The extra weapons' TargetCleared, for the slots from 3 up, is started by `tagpu_weapons.c`, which
records it itself (`tagpu_rec_start_named`).

**BeginTransport is recorded at its start, not at its broadcast** [DISASSEMBLED 2026-10-13]. The
start at `0x4113EF` runs at once (`push 1` at `0x4113E6`): `0x4B0B00` then runs all eight thread
records at `dt` 0 and the piece stepper (`0x4B0B86..0x4B0BA6`), so the script's first part — the
link piece the engine reads right after through `0x43DEF0` (`0x411412`) — happens inside the
order run. The broadcast that follows at `0x411403` makes the remote copies start it through the
`0x10` receiver, whose call `0x455551` passes run-now 0, as the replay's `RK_EV_START` does; a
puppet playing the broadcast ran the script's first part one slice late. The two calls are one
straight-line block (no branch lands in `0x4113D1..0x411402`), so the recorder records the start
and leaves out the broadcast whose return address is `0x411408` (`ev_start_named_bc`). Of the five
broadcast sites this is the only one whose local start runs at once: StartBuilding `0x4385C7`,
StopBuilding `0x43862B` and `0x43A251`, and AutoAim's aim start pass run-now 0, as the receiver
does.

**setSFXoccupy is recorded at its start** [DISASSEMBLED 2026-10-13]. `0x43DB50` has one caller,
`0x43DD65` in the movement pass `0x43DD20`, and that has two: the unit tick's `0x48AFAA`, for a
unit with a mover (`0x48AFA3`) behind the owner gate, and the `0x2C` receiver's sweep `0x48BA46`
over a network peer's units. Neither runs on a puppet level, so a playing writes `+0x10A` from
each image (`tagpu_replay.c`, `water_put`) and never runs `0x43DB50`: the recorded start is the
script's one source. A puppet plays it among the step's replayed events, which come before the
images that place the unit (`0x48A9F0`) and start its walk class (`0x43DA70`); the recording
machine made it last of the three, in `0x43DD20`'s order (placement `0x43DD55`, walk `0x43DD5D`,
water `0x43DD65`). So when one step changes both the walk class and the water state, a puppet
starts `setSFXoccupy` before the walk script, and the start's run-now pass (its third argument 1 at
`0x43DBDD`; `0x4B0B86..0x4B0BA6`) runs the unit's threads at `dt` 0 at the previous step's
position. The owner accepted that order on 2026-10-13 with the walk class's residual (the
completeness note, *Not closed by the marker*).

**Shots: the five network tests.** A fire function builds its `0x0D` only under the network bit:
`test byte [reg+0x2A44],1` then a `je` past the build and the send, at `0x49D7A0` (`ecx`, to
`0x49D85E`), `0x49DA92` (`edx`, to `0x49DB52`), `0x49DC64` (`eax`, to `0x49DD2E`), `0x49DE25`
(`ecx`, to `0x49DEF3`) and `0x49DFAF` (`edx`, the short `je` to `0x49DFFB`, the meteor's). While a
skirmish is recorded, each test's stub leaves the zero flag clear, so the block builds its packet;
between the test and the send each block writes only the packet on its own stack and calls only the
send (the meteor's also `0x44FDB0`, which reads a netid; [the events note](../tadr-port/demo-recorder-s2-events.md)
§1.4). That is the retail code. In Impure's image four of the blocks also run the extra weapons'
slot-index splice (`0x49D7D9`, `0x49DAD5`, `0x49DCA4`, `0x49DE73`), which writes no simulation
state: it counts `SIM_PRIVATE` diagnostics, and the side table it can build for a slot from 3 up is
already built by the splice the same fire call runs before the test (`0x49D6A3`, `0x49D9D4`,
`0x49DB8D`, `0x49DD78`), which takes the same two registers, unwritten in between. The five sends (`0x49D859`, `0x49DB4D`, `0x49DD27`, `0x49DEEE`, `0x49DFF6`) are already the
extra weapons' `wpn_tx_fired`, which hands the packet to the recorder and skips the send when there
is no network. This is the one place the recorder changes what the engine does, and it changes
nothing a skirmish keeps: the packet is the fire function's own stack.

#### What a shot's packet says of its start — `0x49D9C0`, `0x49D270` [DISASSEMBLED + MEASURED 2026-10-09]

**Three of the four unit senders send the start their build used; `0x49D9C0` sends the shooter's
centre.** The builders copy the start they are given to the projectile's position `+0x04` and tail
`+0x10` (`0x49C740`, `0x49C754..0x49C77B`). The turret function `0x49D580` builds from its local at
`[esp+0x18]` (passed at `0x49D776` or `0x49D766`) and copies the same local into the packet at
`0x49D7AD..`; `0x49DB70` builds `0x49CC20` from `[esp+0x10]` (`0x49DC4A`) and copies it at
`0x49DC71..`; `0x49DD60` calls `0x49C740` from its `[esp+0x10]` (`0x49DDD8`) and copies it at
`0x49DE32..`. `0x49D9C0` (flag bits 0 or 20) asks
`0x43E240(unit, &out, slot, −1)` for the firing piece's point into its `esp+0x10` (`0x49D9D9`,
the function's `esp` after its four saved registers), builds `0x49C9C0(slot, unit, &out, target,
target unit)` from it (`0x49DA76..0x49DA7F`), and then fills the packet, at its `esp+0x1C`, with the
unit's `+0x6A..+0x75` as the start (`0x49DA9F..0x49DAC7`: x and y by `0x49DAB6`, z read at `0x49DABD` and stored at `0x49DAC7`) before sending it at `0x49DB4D` (`push
0x24`, `lea eax,[esp+0x20]`, `push eax`, `push edx`: at the call the packet is the call's
`esp+0x28` and the piece point its `esp+0x1C`). The receiver `0x49D270` builds from `msg+1` and
targets `msg+0xD` (`0x49D547..0x49D558`), so on every other peer, and on a replay that plays the
packet as sent, that sender's projectile starts at the centre and not at the piece.

Nothing writes the piece point between the build and the send: `0x49C9C0` reads it (`0x49CA39..0x49CA47`) and hands it to `0x49C740`, which reads it and passes it on to `0x47F300` (`0x49C85C..0x49C866`), which
only reads it (`0x47F372`), and to the emitter `0x4729D0(pos, 9)`
(`0x49CC00`), whose method `0x474D50` (the vtable `0x4FD618`, slot `+0x18`) copies it to its own
`+0x2C`; their stack writes land in their own argument slots, below the function's `esp`; the
network block writes from `esp+0x1C` up; Impure's splice at `0x49DAD5` writes registers and
`SIM_PRIVATE` diagnostics. The recorder therefore records the event's `+1..+0xC` from `[esp+0x1C]` of
the send's frame, after checking the packet is at `[esp+0x28]`, and sends the packet untouched
(`tagpu_patches.c`, `wpn_tx_fired`).

MEASURED (2026-10-09, battle-los, every shot of step *k* against its projectile at capture *k*+1 —
same weapon, spawn GameTime `+0x42` = *k*, attacker `+0x52` — is the shot's `+1` the spawn point
`+0x10`?): from the packet, 8543 of 8543 shots of the other senders' weapons on it and 24 of 24 of `w109`/`w110`
(the `0x49D9C0` weapons of that battle) off it by 4 to 22 px; the puppet's first tick then differed,
and on that recording the slot-B impacts `0x499EB0` from `0x49B27A` were 28 live against 31 on the
playing by GameTime 3031. With the build's start recorded, 8227 of 8227 on it, `w109`/`w110` included, and the
counts over [1021, 2281) equal (slot B 1/1, slot A `0x49B218` 1291/1291).

**Interceptions** need no release: area damage `0x49A120` builds its two `0x0E` with no network
test, and the extra weapons' reads of the weapon's ID byte (`0x49A78C`, `0x49A7CD`,
`wpn_tx_intercept`) hand the recorder the target point and the weapon's index.

MEASURED 2026-09-30, format 2 (`tools/replay/tarec.py check`, which reads every step's events
against the images around it: an appearance needs a create, a disappearance a death, a slot made
again both, a change of `+0x10E` a state event with the new byte, and `+0x104` falling to 0 a build
done):

- an eight-minute skirmish of four AIs on Core Prime Industrial Area, 14,610 iterations: 48
  creates, 51 deaths, 84 broadcast starts, 48 builds done, 56 state bytes, 170 replies and 16,601
  sprays, every change explained, no event off the game thread and no spray uncredited;
- the two-minute battle of 1,432 units on Town & Country, 3,840 iterations: 1,430 creates, 1,433
  deaths, 7,780 shots, 20,632 broadcast starts, 21,873 owner-only starts, 61 state bytes, 16,415
  replies and 24 sprays, every change explained, none off the game thread and none uncredited;
- in the stock-limits build, a three-minute skirmish of four AIs (5,610 iterations) and a
  one-minute run of the battle (2,010: 568 shots and 3,464 owner-only starts), every change
  explained, with all 33 sites in.

Neither game intercepted a projectile, so the interception event has not been seen in a recording.

### The puppet host: its gates, the 30-tick deadline and the sight loop [DISASSEMBLED + MEASURED 2026-09-30]

S2b of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s2b-watching) (`tagpu_replay.c`,
`tagpu_puppet.c`) plays a recording on a level whose recorded seats are type 3. The research is
[the host note](../tadr-port/demo-recorder-s2-host.md), which also says where the build departed from
it. Everything here runs on the game thread.

**The step.** B2's stub at `0x4954ED` calls the unit tick `0x48AD30` and then `tagpu_replay_step`
(the stub's whole order is in [*tick boundaries*](sim.md#the-demo-recorders-tick-boundaries-the-sim-step-0x495490-and-its-callers-disassembled-2026-10-01)):
the events of step T in recorded order, then the image of T+1
([the events note](../tadr-port/demo-recorder-s2-events.md) §1.0). The rest of the iteration then runs
on the recording's world. The level's first step starts earlier, at the first iteration's capture
point (the stub's first call, `tagpu_replay_capture`): the checks, the level's own units, image T,
the wind's seed and V's sight go in there, and then the capture point's write every later capture
point makes (`recorded_put`), before the unit tick, so the units get the slice the recording
machine's first unit tick gave them, drawing COB RAND from the recording's simulation RNG
`0x51FC88` (*COB on a puppet*).

**Player `+0xF0`, the players phase's 30-tick deadline.** `0x464700` sets it, `+0xF4` and `+0xF8`
to GameTime at the level load (`0x464715`, `0x464727`, `0x464765`), for each record with a nonzero
type (`0x464990`: `0x46499E..0x4649B1`). In the players phase `0x464F80`, a record whose `+0xF0`
has reached GameTime (`0x46507D..0x465089`) gets `+0xF0 += 30` (`0x46508F`) and its 30-tick block:
for the controlled record (`0x46509D`) the end tests and the respawn; the economy `0x401360` for
types 1 and 2 (`0x465539..0x46555A`); for the viewed record (`0x465565`) the radar pass `0x467440`
and the minimap fog composite `0x466C20` (`0x46556D`, `0x465572`); and in game type 3, every fourth
pass, `0x4573D0` (`0x465583..0x4655A1`). The viewed record's minimap rebuild `0x466DC0` runs every
tick, before the deadline test (`0x465065..0x465072`). The one other writer is the saved game's
loader `0x466050` (`0x46623F`; its one caller `0x43264E`). A puppet host keeps `+0xF0` itself: the
applied image carries it already advanced, which kept the radar pass from ever running for a
recorded perspective (the host note §2.3).

**The radar pass's sight loop**, the fifth of its five loops (`0x46770D..0x46782F`; back edge
`0x467829`). For each live unit (`+0x110` bit 28) without
bit 8 yet (`0x467722`) and without `+0x10E` bit 2 (`0x46772E`), the viewed record sees the unit's
cell `(x >> 5, (z − altitude/2) >> 5)` of the integer position words, bounded by the record's
`+0x80`/`+0x84` (`0x467786..0x467790`, `0x4677CC..0x4677D6`): under true LOS (LosType bit 1,
`0x46775D..0x467769`) when the record's sight counter there is nonzero, otherwise when the record's
bit is set in the explored map's u16 there (`0x4677EF..0x467808`); a unit it sees gets bit 8
(`0x46780E..0x467814`). The create writes the bits a unit has before any pass
(`0x485C1E..0x485C48`): bits 8–9 cleared, bit 9 set when the owner is the viewed record. MEASURED
2026-09-30: a host that never ran the pass for record 0 showed exactly that on all 1,413 units of the
battle recording, whose own dumps had bit 8 on every unit under LosType 4 (the explored map filled
by `0x4816A0` since bit 0 is clear). Under true LOS the counters it reads hold every live
[temporary sight record](#the-demo-recorders-globals-projectiles-and-seek-disassembled-measured-2026-10-01)'s
stamp too, so for 60 ticks after one of the viewed record's units dies the loop sets bit 8 on enemy
units over the dead unit's sight circle. MEASURED 2026-10-01 (battle-los, a fresh seek to 1111 that
left the viewed record without the one record a playing held, from a death at 1092): its counters
were lower in exactly 213 cells, one radius-8 stamp, and bit 8 was clear on the aircraft over it
that held it in the recording and on the playing; with the seek making the record again, both
compare equal (the slice note's *S3c-3*).

**`Send_UnitDeath 0x4864B0(unit, …)`**, `ret 8`: its callers are the reaper `0x48AFD1`, the
console's kills of a type's units `0x486E80` (`0x486EB2`) and of every unit `0x486ED0` (`0x486EF8`),
kill-all `0x486F10` (`0x486FB4`), CreateFromNetwork's occupant `0x486244`, and the teardown's
destroy-all `0x485980` (`0x4859A8`), whose one caller is `0x491B95` in the teardown `0x491B60`.
Impure's scenario tooling calls it as `UNITS_KillUnit`.

**The apply `0x489CE0(rec)`**, `ret 4`: its direct callers are the sender's local apply `0x489C89`
and the pump's `0x0B` case `0x455412`. The sender forwards to a type-3 owner through `0x451DF0`
(`0x489CB9`), which sends only with the network bit (`0x451EAB`).

**The COB writers are methods of the unit script class** (vtable `0x4FD698`, written at `0x485D8D`
in `0x485D40`): ATTACH `+0x38` = `0x481340`, DROP `+0x3C` = `0x4813B0` and SET `+0x40` = `0x480B20`,
called by the VM at `0x4B1BB4`, `0x4B1B8D` and `0x4B1BD2`; the table holds the exe's only
references to them. SET begins `mov eax,[ecx+0x540]` — the model field, whose `+0xC` is the unit
(`0x480B2B`) — then `mov ecx,[esp+4]` and `push esi` at `0x480B26`. The raised limits move the
model field and rewrite that first displacement (`0x480B22`, `tagpu_patches.c` `cob_runtime_sites`),
so a hook of SET goes at `0x480B26`, where ECX is still the object and nothing is pushed.

**Typed commands `0x417B50(line, mask)`**, `ret 8`: the chat send `0x493E36` tests the result's bit 1
(`0x493E3E`), and the key handler `0x496393` ignores it.

**The gates** (`tagpu_puppet.c`): each byte-matched against these bytes before any lands, all or
none, and no branch in the exe lands inside a stolen range. A gate returns at once, EAX 0, on a
level that plays a recording, and otherwise runs the stolen bytes and goes on:

| Site | Stolen bytes | `ret` | Refused when |
|---|---|---|---|
| `0x489CE0`, the apply | `53 56 8B 15 E8 1D 51 00` | 4 | the victim, the `u16` at `rec+1` bounded by the unit array, is a puppet seat's unit |
| `0x4864B0`, Send_UnitDeath | `83 EC 10 53 55` | 8 | the unit is a puppet seat's |
| `0x480B26`, inside SET | `8B 4C 24 04 56` | 8 | the script's unit is a puppet seat's, outside the replay's own create and reply |
| `0x481340`, ATTACH | `8B 44 24 04 56` | 0xC | the same |
| `0x4813B0`, DROP | `8B 44 24 04 56` | 4 | the same |
| `0x47F780` and `0x47F850`, the replies | `8B 0D E8 1D 51 00` and `57 8B 7C 24 08` | 0xC | the unit is a puppet seat's, outside the replay's own create and reply |
| `0x437DE0`, the meteor shower | `8B 0D E8 1D 51 00` | 0 | always |
| `0x417B50`, typed commands | `81 EC D4 00 00 00` | 8 | always |
| `0x4956C0`, the debug keys (one caller, `0x4965E1`) | `8B 44 24 04 53` | 4 | always |
| `0x464C60`, the resource transfer | `8B 44 24 04 53` | 0x10 | always |
| `0x4326B0`, the save | `81 EC 04 01 00 00` | 0xC | always |
| `0x424050`, the feature tick | `A1 E8 1D 51 00` | 0 | from the level's first step, outside the replay's own calls ([the features](map.md#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30)) |
| `0x423C50`, the spawn | `83 EC 28 53 55` | 0x14 | the same |
| the destroy, `FEATURES_Destroy` | `53 8B 0D E8 1D 51 00` at [`0x4246B0`](memory.md#0x4246b0-features_destroy-the-wreck-path) | 8 | the same |
| `0x423550`, FeatureDie | `83 EC 08 53 55` | 0xC | the same |
| `0x4233A0`, the burn | `8B 44 24 08 8B 4C 24 04` | 0xC | the same |
| `0x4244B0`, the damage | `8B 0D E8 1D 51 00` | 0x10 | the same |
| `0x423710`, the swap | `8B 44 24 08 8B 4C 24 04` | 0xC | the same |
| `0x47BD70`, the skirmish elimination | `81 EC C8 00 00 00` | 4 | while a seek rebuilds the level |
| `0x48B090`, the state setter | `51 8B 44 24 0C` | 8 | on a multiplayer recording's segment, the unit (ECX) is a puppet seat's, the call would change `+0x10E` by the setter's own rule (`0x48B09A..0x48B0D4`), outside the replay's write of the byte from the owner's image |

**What else the apply rests on** [DISASSEMBLED 2026-09-30]:

- **The level start `0x465E30`** runs over all ten records (`edi` `0xD5C..0xD84`, `0x465E41`,
  `0x465F87`), with no activity test, and in a skirmish (game type 2, `0x465E77`) writes each
  record's `+0x8C` and `+0x98` from its skirmish settings entry's `+0x10` and `+0xC`
  (`0x465EF5..0x465F1A`). So the records neither machine seats hold values from the two machines'
  menus, and the apply writes every record but V's.
- **The destructor's order purge** (`0x4867A1`: `0x439EB0(unit, 1)`) sets `+0x42` bit 16 on every
  order node but the head (`0x439EE6`) before it tears each down through `0x43A1F0` and frees it
  (`0x439EF3`, `0x439EF9`). The teardown `0x43A1F0` starts TargetCleared through
  `0x489800(unit, 3)` (`0x43A2B9`) unless the node's `+0x42` bit 16 is set (`0x43A2AB`), after
  zeroing the owned object `+0x52` (`0x43A2A4`). A puppet's nodes are held with bit 16 set, so a
  replayed death starts no TargetCleared; the recording carries the head's own, recorded right
  after its death.
- **The order linker `0x43AD10(unit, node)`** is stdcall, `ret 8` (`0x43AD45`).
- **The restamp `0x47CC30` leaves a carried unit in no bucket**: when `+0x86` is set
  (`0x47CCDF..0x47CCE7`) it writes only `+0x82` (`0x47CD25`) and skips the unlink and the relink,
  so a set-position on a cargo keeps its chain.

**The facts `tagpu_replay.c` cites besides** [DISASSEMBLED 2026-09-30]:

- **Opening.** The Start calls the settings persist `0x430F00` at `0x47B09C`, after the seat fill
  (`0x47B092`). The loader builds LosType bits 0–2 from the skirmish settings into `main+0x14281`
  (`0x4973E0..0x497450`), and its crash handler is `0x497CAB`. `0x4816A0` fills each type-1–3
  record's sight counters, `+0x88` bytes at `+0x7C` (`0x481734..0x481767`), and the explored map,
  `(W·H·2) >> 2` bytes (`0x4816B5..0x4816D2`).
- **Order nodes.** `+0x42` bit 21 (`0x200000`, loaded at `0x439768`) is set with the target's
  last-seen place in `+0x32`/`+0x34` (`0x4397AF..0x4397C5`). A descriptor's marker mask `+0xC`
  bit 0 draws the build site (`0x439B87`). The constructor `0x43A0C0` writes the vtable `0x4FD2CC`
  (`0x43A0DA`) and then `0x4FD2C8` (`0x43A131`), which every constructed node keeps.
- **The placement part `0x43D6D0`** adds the vertical velocity to the altitude
  (`0x43D80D..0x43D813`), after the x (`0x43D80B`).
- **The footprint.** The create copies the def's `+0x14A` into the unit's `+0x7E`, the footprint's
  width and height (`0x485AA4..0x485AAA`); the refusal `0x47D970` reads its cell x and z at `+0x76`
  (`0x47D98E`); YARD_OPEN reads the def's yardmap, a byte a footprint cell at `[def+0x14E]`
  (`0x47DA3D..0x47DA51`), and sets `+0x110` bit 27, the re-claim owed (`0x47DAEC`); the re-claim
  `0x47C790` sets bit 26 on the incumbent of a cell it contests (`0x47C828..0x47C89C`).
- **The events.** The build done `0x41B8D0` tests both units, and each one's `+0x110` bit 28, before
  it writes (`0x41B8D6..0x41B917`). The `0x10` receiver's case calls the start with the unit's COB
  `+0x9A` and the message's arguments (`0x455528..0x455551`). The nano emitters index the ten
  16-byte particle layers at `[main+0x38D77]` with their layer word, unbounded
  (`0x47217C..0x472188`). The shot receiver `0x49D270` reads the shooter's mover's speed `+0x20`
  (`0x49D50F`). The notice function `0x47FAD0` indexes its table `0x5086D8`, 24-byte rows, by the
  kind, unbounded (`0x47FBA4`, `0x47FBB1`, `0x47FC33`). The create `0x485F50` indexes the owner's
  record unbounded (`0x485F5C..0x485F78`); a nonzero slot argument is taken exactly and an
  occupied one refused (`0x486002..0x486043`), and 0 takes the first free (`0x486036`).
- **The attach `0x48AB70(rec)`.** The parent's `u16` is at `rec+1`, 0 to detach (`0x48AB7D`); the
  piece at `rec+5` goes to the child's `+0xF9` (`0x48AC63..0x48AC68`), and the layer at `rec+6`
  into bits 0–1 of the child's mover's `+0x2E` (`0x48ACE3..0x48ACF0`, the mover `[unit+0]` at
  `0x48ACE1`). A
  child already carried is first unlinked from its carrier's chain (`+0x86`, `+0x8E`;
  `0x48AC23..0x48AC53`), and an attach pushes it at the front of the new one (`0x48AC7C..0x48AC82`).
  The order purge `0x4384A0` runs only for a child whose owner's record is active and of type 1 or
  2, attached to a parent whose def's `+0x241` bit 9 is clear (`0x48ACF3..0x48AD1F`). The child is
  then deselected: `0x48C9B0`, whose one call is `0x48AD25`, tests its `+0x110` bit 4.

The recorder's observers on the two replies chain onto their gates, so they see only the replies
the gates let through. MEASURED 2026-09-30, the 1,432-unit battle in full visibility: 252
Send_UnitDeath calls, 26 SETs, 37 replies and 3,809 meteor showers refused, and no hit, ATTACH or
DROP. A puppet's shots do no damage by the impact's owner gate (B7), and the reaper is the one
caller of Send_UnitDeath that runs on a skirmish host [INFERRED from the callers' conditions: the
kill-all needs a type-1/2 owner, CreateFromNetwork a network game].

**What the playback's bounds and its settings observer rest on** [DISASSEMBLED 2026-09-30]:

- **The settings save `0x430F00`** (`REGISTRY_SaveSettings`) takes no argument, begins
  `A1 E8 1D 51 00` (`mov eax,[0x511DE8]`), nothing jumps into its first five bytes, and it returns
  at `0x43173F` with what its last `0x4B6A50` returned (`0x431734`): the DWORD writer's result,
  which `0x4B6880` leaves in EDI (1 on the paths at `0x4B693E` and `0x4B696B`) [its meaning
  INFERRED]. It writes `main+0x37EEE` (`0x4311D1`), and the skirmish settings `+0x108`
  `SkirmishCommanderDeath`, `+0x10C` `SkirmishMapping`, `+0x110` `SkirmishLineOfSight`, `+0x114`
  `SkirmishLOSType` and `+0x118` `SkirmishLocation` (`0x4314F6..0x43157E`; `+0x118` is read at
  `0x430F80` too), then `+0x228`
  `SkirmishDifficulty` (`0x4315A0`), `SkirmishMap` and `Player%dController` (`0x4315C9`,
  `0x4315F6`). Its callees are `0x4B6A20`, `0x4B6A50` (58 calls), `0x4CF220`, `0x4CFFF0`,
  `0x4D0040` and imports (`call ebx` is `[0x4FC2E8]`, `0x4315E9`), none of which reaches it again.
  No call of it is on the loader thread: the loader's own, `0x497FF6`, is in the game thread's load
  state `0x497F40`, which starts the loader thread only after it (`push 0x497C70; call 0x4B6B20`,
  `0x4982C5..0x4982CA`).
- **What its 27 callers do with EAX.** Sixteen are in typed-command handlers (`0x416420` to
  `0x4173E0`, named by the 12-byte rows of the command tables `0x501D38`, `0x501F48` and
  `0x501FD0`, which `0x4195A7..0x4195D8` register through `0x4B7760`); fourteen of those call it
  last (`call 0x430F00; ret 4`) and so return its EAX [what the dispatch does with it: not
  traced], and `0x416D6F` and `0x416D9F` call `0x4816A0` after it. The battleroom's three (`0x446246`, `0x44626C`, `0x4462FC`, in a handler at `0x4461D0` that no
  direct call or pointer in the image names [OPEN]) and `0x477C92` (`0x477CAC..0x477CB0`) return
  it too; `0x426E80`'s (`0x426F65`) reaches `0x496BB5`, which overwrites it; the rest overwrite or
  ignore it. A save that is skipped therefore changes what GUI handlers report to their callers,
  which is why the playback observes it (`tagpu_detour_observe`) instead of gating it.
  MEASURED 2026-09-30, the reference setup, Wine: Options' OK during a playback reached the save,
  and a build without the observer wrote the recording's `SkirmishCommanderDeath` 0 and
  `SkirmishLocation` 1 over the player's 1 and 0.
- **The in-game key switch** at `0x495ECD` indexes the byte table `0x496694` with the key code
  less 9, at most `0xEF` (`0x495EB7..0x495EBF`). Codes `0x21`, `0x23`, `0x2A`, `0x60` and `0x7E`
  (jump slots 3, 4, 5, 15 and 19) reach `0x496058`, which flips `main+0x37F06` bit 0 and saves
  (`0x496077`). MEASURED: an injected `VK_END` does not
  reach it, the word stayed `0x3F` [INFERRED: the table's codes are the engine's own, as Ctrl+A's
  `0xAA`, not virtual keys].
- **A list sized by the player count.** At `0x4938B0..0x4938CC` a function whose entry is not
  established allocates `main+0x2A3C` × `0x1E` bytes (the u16 count at `0x4938B7`, the allocator
  `0x4D83B0`) and walks the ten records (`0x4938FF..0x4939BA`): active (`[rec+0]`), of type 1, 2
  or 3 but not 1 (`0x493915..0x493922`, `0x493945`), `+0x146` not `0xA`, `+0x144` nonzero or
  `+0x140` zero, and not a watcher (`0x49394C`). Each one's name at `+0x2B` is copied to its NUL,
  packed (`0x493955..0x493977`), and its `+0x4` into a second array. A count below the records it
  takes, or a name of `0x1E` bytes or more, runs past the list.
- **Sides and colours are indexed unbounded**: PlayerInfo `+0x95` into the side table
  `main+0x37F5F`, `0x232` bytes a side (`0x48692A..0x486943`), and `+0x96` into
  `[main+0x148DB]` (`0x467C18..0x467C24`, every frame). The engine's own bounds are
  `0x4B7F30`'s `0 <= i <` the u16 count at the colour sequence's `+0` (`0x4B7F36..0x4B7F46`), and
  the lobby's clamp of a side at or above the dword `main+0x37F39` to 0 (`0x447EC0..0x447ECE`).
  `main+0x148DB` is loaded at start-up from `0x4B8D40` (`0x431911`). The engine itself leaves colours
  outside the count in records it does not seat: `0xFF` from the battleroom (`0x45133E`), and the
  record's index from the reset below (`0x46461E`).
- **The seat fill `0x47A760`**, per settings entry (`[main+0x29A0]`, `0x18` bytes, the count
  `main+0x38D81`): type 1 writes the entry's colour (`+0x14`) and side (`+4`) to PlayerInfo `+0x96`
  and `+0x95` (`0x47A79F..0x47A7C1`), calls `0x464290(i, 1)` and sets `main+0x2A43` and `+0x2A42`
  to `i` (`0x47A7D2`, `0x47A7DE`); type 2 writes the same two (`0x47A7F2..0x47A813`) and calls
  `0x464290(i, 2)`; any other `0x464290(i, 0)` (`0x47A81B`).
- **The watcher bit**, PlayerInfo word `+0x9B` bit 6. The seat activation clears only bit 5
  (`0x464378`). The front end's reset `0x4644D0` zeroes each player record but its `+0x27`
  (`0x4644DD..0x4644F6`) and each PlayerInfo (`0x46458B..0x464598`), then clears bits 4 and 5
  (`0x464600`, `0x4646C2`), sets the colour to the record's index (`0x46461E`) and the side to 0
  (`0x46462C`); its three calls are `0x42734F`, `0x427D73` and `0x4518AC`. The word's other
  writers, in the battleroom's `0x443xxx..0x449xxx`, are not traced. The post-game table
  `0x41DC20` leaves a watcher out (`0x41DD43`) and copies a listed player's name with
  `strncpy(dst, rec + 0x2B, 0x1E)` (`0x41DD61..0x41DD6B`); the results screen reads a listed
  player's colour from its PlayerInfo (`0x41E5CD..0x41E5DC`), and the controlled player's watcher
  bit only while the win flag `main+0x391AF` is set (`0x41F347..0x41F35B`). The post-game table
  copies that flag from `main+0x3923B` bit 4 (`0x41DC2E..0x41DC3C`), whose setters are +IWin's
  `0x416A0A` and the players phase's `0x4658C0`, after the objective's test (`0x46586F`).
  MEASURED 2026-09-30: a surrender to the main menu runs no reset (record 0's name was still in
  place at the menu); V (record 4) held `0x54` during a playback, and a build that logged the
  playback's restore showed it put back 0 after the teardown.

### The demo recorder's globals, projectiles and seek [DISASSEMBLED + MEASURED 2026-10-01]

S3b of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s3b-the-globals-and-seeking)
records the main block's and `.data`'s simulation fields through a positive list
(`tagpu/ddraw/replay-globals.tsv`), the projectile pool and the explored map every iteration, and
rebuilds a puppet level in place at any GameTime of a recording (`tagpu_replay.c`, *THE SEEK*).
This is what that rests on, and what the work read on the way. Every list of writers or readers
below comes from a search of every instruction whose operand carries the field's displacement or
address and a scan of the whole image for a function's address as a stored dword, each hit read in
context; a store through a pointer formed earlier was found only by reading, and block copies and
indirect calls are outside that search, so a "none" is a none for direct references.

**The tick, read for "carried into a later iteration"** (`0x495490`): GameTime `+= 1` (`0x4954C0`),
the network pump `0x453D40` (`0x4954C8`, network games only), the unit tick `0x48AD30` (`0x4954ED`,
B2's stub: the replay's capture-point write, then the recorder's capture), the projectiles
`0x49B720` (`0x495513`, its only caller), explosions and flying pieces `0x420F30` (`0x495539`), the
players phase `0x464F80` (`0x49555F`), the features `0x424050` (`0x495585`), `0x415B30`
(`0x49558A`), the wind `0x490C40` (`0x49558F`), the meteors `0x437DE0` (`0x495594`), the camera
`0x41CA10`, the particles `0x471EB0` (`0x4955BF`) and `0x466580`, looped `main+0x38A3B` times; then,
once per call of `0x495490` even when it ran no iteration (`0x4954A2` jumps there), the chat expiry
`0x463EF0` (`0x495683`) and the temporary sight records' expiry `0x482130` (`0x495688`).

**The two images, and why neither is written back whole.** The main block is `0x3924D` bytes from
`main`, allocated once by `0x41D920` (one caller, `0x49E872`) with the GetTickCount pad
([`main` is deliberately MISALIGNED](memory.md#main-is-deliberately-misaligned-and-it-is-redrawn-at-every-launch-mapped-by-us)),
and `main+4`/`+8` hold `.data` addresses (`0x50292C`, `0x502920`, stored at `0x41D970`). `.data` is
`0x501000..0x52B65C` (the section header's virtual size `0x2A65C`; its raw size `0x10A00`, which is
what `objdump -h` prints, so `0x501000..0x511A00` comes from the file and the rest is zero-filled),
and it holds the main pointer `0x511DE8` itself. A dump carries both whole (`RK_IMAGE`); a recording
carries the listed fields.

**The positive list's fields** (each a row of `replay-globals.tsv`, which carries every writer):

| Field | What | Written by | Read by, in a later iteration |
|---|---|---|---|
| `main+0x38A47` GameTime | the sim tick, i32 | the tick `0x4954C0`; the shell's network clock `0x44A696`; zeroed by the level init `0x491979`, the loader `0x4971BB`, the load wait `0x498180` | 166 instructions in 79 functions; every deadline below |
| `main+0x37EC4` | the wind's next draw, a GameTime | `0x490C8E` (`mov [esi],eax`, `esi` formed at `0x490C5A`; Impure's `wind_draw` stores in its place), 0 at load (`0x4918FD`) | the wind `0x490C45` only |
| `+0x37EC8` | 5000, the divisor | the load `0x4918ED` only | the wind `0x490D46`, the AI `0x409B90` |
| `+0x37ECC`, `+0x37ED0`, `+0x37ED4` | the wind's x, altitude and z components | `0x490D09`, none, `0x490D35` (`+0x37ED0` stays as the zeroed block left it [INFERRED]) | the fire spread (`0x423AA1`, `0x423AC1`), the projectile pass (`0x49BC5D`, `0x49BD0A`, `0x49BD20`, `0x49BD28`), the particles |
| `+0x37ED8` u16, `+0x37EDA` | heading, speed | `0x490CDC`, `0x490CB7` (and Impure's `wind_draw`) | the wind; the unit tick's COB call `0x437910` (`0x437941`, `0x437973`) |
| `+0x37EDE` f32 | speed/5000, capped at 1.0 | `0x490D4C`, `0x490D6B` | the economy `0x40156F`; `0x488F68` for the AI |
| `+0x37EE2` | changed this draw | `0x490D7B` (1), `0x490D87` (0) | `0x437933`, which starts COB `SetDirection`/`SetSpeed` once a unit |
| `main+0x39239` i16 | the game end's countdown, −1 idle, 4 when a test fires | the load wait `0x498199` (−1); in the players phase `0x4650FF`/`0x46510C`, `0x46518B`/`0x465198`, `0x4655F4`/`0x465604`, `0x46588D`/`0x46589A` | the players phase only; `0x46554F` skips the economy while it is ≥ 0, and `0x46517F..0x4651A4` runs the end action when it goes below 0 |
| `main+0x3923B` u16 | the game end's flags: bit 2 stop, bit 4 won; bits 0, 1 the debug display and mode; bit 3 set by a save's load (`0x492636`, no reader found); bits 5, 6 won and lost, for the status icon only (`0x46A174`, `0x46A1A2`) | bit 2: +IWin `0x416A22`, +ILose `0x416A7C`, `0x450EA6`, the pump `0x455148`, `0x457961`, the in-game menu `0x46061F`, the "Continue watching?" dialog's CHOICE2 `0x464E55` (handler `0x464DE0`, stored as an immediate at `0x464F59` and `0x4657CA`), the players phase `0x465126`, `0x46561A`, `0x465833`, `0x4658B4`; bit 4: `0x416A0A`, `0x4658C0`; the level init clears bits 2, 4, 5, 6 (`0x491A06`, `0x491A14`, `0x491A22`, `0x491A30`) | bit 2 the economy gate `0x465546`; **`0x4995FC`, in the in-play handler `0x499200`** (stored into `main+0x391F5` at `0x490BC5` and `0x498455`): `test al,0x14`, then the teardown `0x491B60` and ENDMSN |
| `main+0x14281` bits 0–2 | LosType: Mapping, true LOS, the ray fan | the typed commands LOSType `0x416690` (`0x4166AA`), LOS `0x416D50` (`0x416D68`), Mapping `0x416D80` (`0x416D98`), NowISee `0x417540` (`0x417547`, `0x417555`), each then `0x4816A0`; the network watcher switch `0x4656B5`, `0x4656C5`; MakePoster `0x495A30` clears and restores bits 0, 1 (`0x495B36`, `0x495B58`, `0x495E30`, `0x495E51`); the loader's three branches | the sight stamp, the destructor (`0x486838`), `UnitInPlayerLOS 0x465AC0`, the site test, the AI; bit 3 (the fog grid is current) is the draw's, set at `0x484904` |
| `0x51FC88` | the simulation RNG, Park–Miller (`0x4B6C30`, `0x4B6CA0` the seed) | `0x4B6C87` (each draw), `0x4B6CAB` (the seed, called once, by the loader at `0x49719D`) | `0x4B6C41`; 129 call sites of the draw |
| `0x5122E8`, `0x512318`, `0x51231C`, `0x512320`, `0x51232C`, `0x512330`, `0x512334` | the meteors: the next shower, active, its end, its second point, enabled, the next strike, its first point | the init `0x437CD0`, enable/disable `0x437D40`/`0x437D50`, the tick `0x437DE0`, the forced shower `0x438070`, the save's loader `0x438250` | the tick `0x437DE0` |
| `0x5119E8 + 4i`, `0x511A10 + 4i` (i < 10), `0x511A38` | the pathfinder's per-player budget, searches started, and a call counter mod 150 | the budget `0x40EAFD` (the init `0x40E9E0`), `0x40EBDE`, `0x40EBFB`, `0x40EC06`; the searches `0x40ECF9`, `0x40EC0C`; the counter `0x40EBB0`, `0x40EBB7` | `0x40ED76` (a search's start), `0x40EBBF`, `0x40ECE4`, `0x40EBA5`; the tick `0x40EB70` (from the players phase, `0x464F92`) runs for types 1, 2 and 3 |
| `0x512358 + 0x20k + 0x1C` (k < 32) | each movement class's watermark | `0x440B18` alone, `max(GameTime, 30) − 30`, in `0x440AF0` (one caller, the pathfinder's search start `0x40E630`, `0x40E655`) | the claim `0x440A70` (`0x440A94`: a unit whose claim stamp is below it re-marks its footprint into the class's grid) and `0x440B06` |
| `0x51E53C` | the `0x28` resource broadcast's counter | `0x465595` | `0x46558D`; network games only (`0x435100` = 3 at `0x465588`) |
| `0x51E684`, `0x51E688` | the build-site test `0x47D2E0`'s outputs | `0x47D2EE`, `0x47D7FB`; `0x47D2F4`, `0x47D4E7` | the getters `0x47C780`, `0x47C770`; only `0x51E688` at `0x40A76A` (AI) reads a value an earlier call left |
| `0x51E6C4` | the objective's GameTime timer | `0x4903AC`, `0x4903BF` | `0x49037C`, reached only while `0x511DE4` is nonzero, which the CD search `0x41D6A0` never sets in this exe (`0x50289C` is 1 in the file and has no writer): **always 0** [INFERRED: a copy-protection timer] |

**Not reset by any load**: the movement classes' watermarks (`0x440940` rebuilds the grids and never
writes `+0x1C`, so after a level change the engine already runs with a watermark above GameTime),
the pathfinder's counters `0x511A10..0x511A3B` (only `0x40EB70` writes them), the meteors'
`0x51231C`, `0x512320`, `0x512330` and `0x512334` (every shower's start writes all four before they
are read, so only the save's writer reads them stale), and `main+0x38C5B`. A fresh process and the
recording process hold different bytes there at the same GameTime, which is why they are recorded.

**What the list leaves out** is named byte for byte by the TSV's `leave` rows, each with its writers
and evidence: LosType bit 3; what the tick writes for the viewer (the camera and eye `+0x142F3`,
`+0x142F7`, `+0x1431F..+0x1434E`, `+0x14371`, the radar blink `+0x142EF`, the redraw flags
`+0x142F1`, the minimap `+0x1436B`, OFFSCREEN `+0x37E1B`, the GUI `+0x37E9C`, `+0x37EBE`,
`+0x2AA7..+0x2CC6`, the game speeds `+0x38A4B`, `+0x38A4D`); the wall clocks (the pacing gate, the
profiler `+0x38D85..`, and the rest of
[*every wall clock the engine stores*](sim.md#the-demo-recorders-counts-and-the-steps-phases-sinks-impacts-emitters-and-the-images-gates-map-disassembled-measured-2026-10-01));
caches of the draw; in `.data` the network statistics `0x511A40..`, chat, the network object
`0x513000..0x51E53B`, the DLL entry points, the music's combat ring `0x51E710`, the scoreboard, the
timers, the libraries' and the C runtime's state and per-call scratch; and the level's constants
(the wind's minimum and maximum `+0x1425B`/`+0x1425F`, gravity `+0x14263`, the sea level
`+0x1427F`, the map's size, the unit limit `+0x37EE6`, the difficulty `+0x37EEE`,
ActiveCommanderDeath `+0x37EF6`, the meteor config `0x5122F0..0x512338` less the seven fields).
The CRT's `rand` is per thread and in neither image.

**The projectile pool** (base `*(main+0x141F7)`, count `main+0x141F3`, `0x6B` bytes a record).
`0x499A30` allocates `0x7D64` bytes, "WEAPON ARRAY" (`0x499A3B`; 300 records exactly, 3000 under the
raised limits), stores the base (`0x499A49`), zeroes the block (`rep stos` of `0x1F59` dwords from
`0x499A55`) and the count (`0x499A6A`); its one caller is the level load (`0x4918B6`).
`0x499A80` frees it and nulls the base (`0x499A8C`, `0x499A9A`; one caller, `0x491C30` in the
teardown cascade). The fields (init = `0x49C740(proj, weapon, start, target, time, attacker)`,
stdcall `ret 0x18`; a field init does not write keeps the bytes of the slot's last tenant):

| Offset | What | Written at creation | Written later |
|---|---|---|---|
| `+0x00` | the weapon | init `0x49C752` | — |
| `+0x04..+0x0F` | position x, altitude, z (16.16) | init | the tick: the velocity and the wind |
| `+0x10..+0x1B` | the tail; for weapon bit 25 the guidance's waypoint | init | the tick, the guidance |
| `+0x1C..+0x27` | velocity | the builders; a meteor's from its message | the guidance, gravity, a burst's spray, a bounce |
| `+0x28..+0x33` | the target point | init, **only when the target argument is not NULL** — stale for ballistic, bit-8 and meteor shots | — |
| `+0x34`, `+0x36`, `+0x38` | spin, heading, pitch | heading and pitch by the builders | the tumble, the guidance |
| `+0x3A`, `+0x3E` | speed; the distance at fire (`0x49C9C0` only, `0x49CA8D`) | the builders | the tick |
| `+0x42`, `+0x46`, `+0x4A` | the spawn, death and next-smoke GameTimes | init `0x49C7AC`, the builders, init `0x49C7C9` | a burst template and its copies |
| `+0x4E` | the target unit | 0 by each append and init (`0x49C7CC`), then the builders' argument | 0 at a bit-24 weapon's second stage |
| `+0x52` | the attacker, 0 for a stone | init `0x49C7DB` | nothing |
| `+0x56` | the quarry: **a record of the same pool** | 0 by init (`0x49C7C6`); the interceptor builder `0x49CC20` (`0x49CD25`) | the compaction (`0x49AF4B`) |
| `+0x5A`, `+0x5C`, `+0x5E` | the last feature cell hit, the ground under it | never | the collision `0x49B090` |
| `+0x60` | a burst's shots left; nonzero is an invisible burst template | init 0, the builders | the tick |
| `+0x62` | the firing piece | init, with an attacker (`0x49C833`) | Impure's B7 for a received stone |
| `+0x64` | a flame's spin | never | the tick |
| `+0x66` | the owner, 10 without an attacker | init | — |
| `+0x67` | the record's index before this compaction | — | the compaction (`0x49AE5F`); the recorder's pool frame keys a survivor by it (*The demo recorder's pool frame*, below) |
| `+0x69` | bit 0 a weapon stage, **bit 1 done**, bits 4–5 a bit-24 weapon's stage | init, the append | the removers |

- **Which pointers dangle.** `+0x4E` and `+0x52` name unit slots (Mode B): nothing clears them when
  the unit dies, and `0x49A792`, `0x49A7D3` and `0x49D3FA` dereference `+0x52` with no NULL test.
  `+0x56` routinely names the wrong record after a compaction (below), but always one inside the
  allocation. `main+0x142F7`, the camera's followed projectile, is a pool pointer outside the pool.
- **The ten appends**, each `cmp count, cap / jge refuse`, then `count += 1`, `+0x69 &= ~2`, `+0x4E =
  0`: the stand-alone helper `0x49B6E0` (**no caller**), the burst copy in the tick (`0x49B809`, a
  `rep movs` of the template at `0x49B844`), the builders `0x49C9C0`, `0x49CC20`, `0x49CDE0`, the
  bit-8 twin `0x49D000` (**no caller**), the receiver `0x49D270`'s meteor and bit-8 branches, the
  bit-8 handler `0x49DD60` and the meteor spawn `0x49DF10`. Init has eight callers (`0x49CA32`,
  `0x49CC90`, `0x49CE4A`, `0x49D064`, `0x49D307`, `0x49D4FC`, `0x49DDE0`, `0x49DF7D`), and every
  append is followed on its only success path by init or by the burst's whole copy, so every
  written slot holds a non-NULL weapon and **the written slots are exactly `[0, peak)`**: the
  recorder finds the extent by scanning forward from the last one while `+0x00` is not NULL.
- **Removal is a mark and a compaction**: `+0x69 |= 2`, then `0x49AE20` (no argument, `ret`;
  callers the tick's end `0x49BE53` and `0x49C880`). It slides the survivors down in order (`rep
  movs`, `0x49AEE0`), stamps `+0x67`, re-aims `main+0x142F7` (`0x49AE84..0x49AE8C`) and writes the
  count (`0x49AF76`); **the slots from the new count to the old keep their bytes**. Its re-aim of
  `+0x56` covers only a mover's link to a surviving quarry: **a record before the first hole keeps a
  link to a quarry that moved, and a link to a removed quarry is never re-aimed** — it names
  whatever slid into the slot. The recording carries that staleness as stock leaves it.
- **The death purge `0x49C880(unit)`** (`ret 4`, one caller, the destructor at `0x4867B5`): each burst
  template whose `+0x52` is the unit is marked done, the camera's follow handed over
  (`main+0x1433F`, `main+0x1434B` = the weapon's `+0xFE`, `main+0x142F7` = 0) and the pool
  compacted **inside the walk** (`0x49C8F4`); the walk then steps past the record that slid into the
  removed one's place (`0x49C904..0x49C905`), so a second adjacent template of the same unit
  survives it.
- **The RNG in the projectile code**: the simulation's at each burst copy (`0x49B8D1` the death time's
  decay, `0x49B903` the spray, which sprays the *template*), and in the bit-19 handler `0x49D580`
  (`0x49D727`, `0x49D733`, the owner's machine only); the CRT's in the draw pass `0x49BE60` and the
  emitters. `0x499E50` (the camera hand-over and the mark without the detonation), `0x49D0C0` and
  `0x49D1E0` have no caller either.

**`0x4816A0(arg)` `Game_SetLOSState`** (stdcall, `ret 4` at `0x48192B`; `arg` tested only for
zero, `0x4816A7`). With `arg` ≠ 0 it fills the whole explored map (`0x4816F4`, `0x4816FB`) with
0x00 when LosType bit 0 is set and 0xFF when it is clear, every player at once. Always, it fills
the sight counters (`[rec+0x88]` bytes at `[rec+0x7C]`) of each record 0..9 (`0x48177A`) that is
active, of type 1, 2 or 3 and whose `+0x146` is not 10 — **a puppet host's V among them** — with 1
outside true LOS and 0 under it. Under true LOS only, it zeroes every live unit's height byte
`+0xF8` and re-stamps it in-line (the ray fan through `0x4825B0`, the circle through `0x482270` and
`0x481930`). Then `main+0x142F1 |= 4` (`0x481905`), LosType bit 3 cleared (`0x481911`), and
`0x466C20` and `0x466DC0` (`0x48191A`, `0x48191F`). It touches neither the terrain sight grid nor
the temporary sight records. Its nine callers are the four typed commands (LOSType, LOS with 0;
Mapping, NowISee with 1), the Deathmatch respawn `0x4654EF`, the watcher switch `0x4656CE`,
MakePoster twice (`0x495B61`, `0x495E58`) and the loader `0x497AE5`: **every change of bits 0–2 is
paired with it**, and a write of the bits without it would remove stamps in another mode than they
were made in.

**The explored map** `*(main+0x14273)`: allocated by LoadMap (`0x483CF6..0x483D25`, "MAPPED MEMORY",
`(W·H·2) >> 2` bytes with W, H the grid's 16-pixel columns and rows, zero-filled, stored at
`0x483D18`), freed by the map-free `0x483DD0`. One u16 per 2 × 2 cells, the bit `1 << [player+0x146]`
(`0x48194B..0x481951`). Its writers: the fill above, the per-tick setter **`0x481930`, which tests
the bit before every store** (`0x481A56`, `0x481B64`, `0x481CD1`) and so only ever sets one, the
share dialog's `0x485420(from, to)` (an OR), and the saved game's `Mapping` loader `0x484FA0`. **It
only grows in play**; it is cleared only by `0x4816A0` with bit 0 set. With bit 0 set the map starts
unexplored and each unit's sight sets bits; with bit 0 clear it is 0xFF from the load and never
written. The simulation reads it: the transport unload `0x47E2D0` tests the unit owner's bit
(`0x47E37B`), the AI's map `0x40D7B0` its own player's, and `UnitInPlayerLOS 0x465AC0` (outside true
LOS) and the order resolver `0x43F0E0` the **locally viewed** player's.

**Per-origin multiplayer capture, DIS / SOURCE:** the allocation computes its size in
32 bits: `0x483CF6/0x483CF9` read W/H at the map block's `+0x38/+0x3C`,
`0x483CFD` doubles the product and `0x483CFF` shifts it right twice before
`0x4D83B0` at `0x483D08`; `0x483D1E..0x483D25` zero both whole dwords and
remaining bytes. `tagpu_recmp_explored_cells` requires positive 16-bit dimensions,
W·H ≤ UINT32_MAX/2 and W·H divisible by four. Thus its W·H/4 bounded u16 reads
fit the exact engine allocation and the doubled product cannot wrap. The stamp
loads the player's self row at `0x48194B`, shifts the mask at `0x481951`, reads
the map at `0x481A48` and the indexed u16 at `0x481A53`; the tested-zero branch
stores that bit at `0x481A67`. These instructions establish the source row's
bit identity, not a remote peer's row order.

`mp_explored_capture` freezes `main+0x14273`, W/H (`+0x14233/+0x14237`) and
copies each local controller-1/2 row's own bit into an origin-keyed bitmap at B2.
Map allocation and level teardown belong to this game thread; equality tests
detect a changed level and are not the memory lifetime guarantee. The private
codec owns copied storage and publishes only a complete BEGIN/END transaction.
It performs no engine write or patch. The full u16 LosType at `main+0x14281`
travels with that origin's bitmap, including housekeeping bit 3. The typed
`Mapping` handler `0x416D80` reads it at `0x416D86`, toggles only bit 0 at
`0x416D8F..0x416D98`, calls `0x430F00` at `0x416D9F` and rebuilds with argument
1 through `0x4816A0` at `0x416DA6`. This inspection establishes no broader
contract for `0x430F00`. A Mapping mode change may clear or fill the map;
the contribution delta therefore preserves both cleared and set bits.

**The sight counters' bookkeeping.** A unit keeps its last stamp's cell in `+0x7A`/`+0x7C` and its
height byte (ray fan) or radius (circle) in `+0xF8`. The stamp `0x482270` increments, the removal
`0x481D50` decrements the same cells, read from the same stored words, with no zero test. The
create zeroes them (`0x485C0A`, `0x485C0E`, `0x485D04`) and stamps fresh (`0x482AC0` at `0x486178`);
the set-position re-emits (`0x4827B0` at `0x48AAA0`: the old stamp removed, the new made); the
destructor removes (`0x482090` at `0x486845`, under true LOS). So after every unit is killed and
made again the counters are the base fill plus the new units' stamps, **plus any live temporary
record's stamp** — and with the ray-fan edge left open: after `+0xF8` is zeroed, a unit at an
unchanged cell whose `def+0x170 + y` is 5 or less is not stamped (`0x48261F..0x48262E`) but a later
removal still decrements [DISASSEMBLED; whether any unit type reaches it is not established].

**The temporary sight records** (`main+0x14277` of them, at most 20, the block `*(main+0x1427B)`,
36 bytes each, "EYEBALL MEMORY"): only the destructor adds one (`0x482910` at `0x486748`), only
under true LOS (`0x482920`), only when the dying unit's owner is the **viewed** player, expiring 60
ticks later (`+0x1C`, `0x4829C4`); while it lives it holds a stamp in that player's counters. They
leave only through the expiry `0x482130` (no argument, plain `ret`), which removes each record with
`+0x1C` below GameTime through `0x481D50` (`0x482161`; the compare is unsigned, `cmp ecx,edx; jae` at
`0x48215C..0x48215E`, so at GameTime 0 it removes none), compacts the rest and writes the count
(`0x482260`). **The engine has no mid-level way to drop them all**: the count is zeroed only by
LoadMap (`0x483D92`) and by `0x481500`, which, like the block's free `0x481530` and the predicate
`0x482110`, has no caller. Setting the count to 0 would leave each record's stamp in the counters
for good. The seek drops them by setting every `+0x1C` to 0 and calling `0x482130` (R4, after its
kills, whose destructor calls add up to 20), and refuses to run at GameTime 0, where that removes
nothing [DISASSEMBLED]. It then makes the viewed record's again through the engine's own add
`0x482910` (R14a, after R13's LosType and before the radar pass): one for each death of a live
viewed-seat unit under true LOS that the recording carries and a playing still holds at step T−1
(steps T−62..T−1, less those a LosType change dropped), from the victim's image as the destructor
found it, with the ticks it has left as the duration, and runs the
expiry once after the radar pass (R14c), as `0x495688` follows the players phase. The add's
refusals are what keep that call safe: under LosType bit 1 clear or at a count of 20 it returns
before writing anything (*The sight emitter*, below), and the seek counts a refusal.

**GameTime deadlines the recording does not carry, and how the engine resets them.**

- **A player's `+0xF0`**, the players phase's 30-tick deadline (`0x46507D..0x465092`: when it is at
  or below GameTime, `+= 30`): the player init `0x464700` zeroes it at GameTime 0 (`0x464715`; one
  call chain, the level init `0x4919C8` → `0x464990` → `0x4649B1`), and the loader's players phase
  `0x497C10` sets every active record's to 30, so **`+0xF0` is always the next multiple of 30 above
  GameTime**. V on a puppet host is a type-3 record in the phase's loop; its pass runs the radar
  pass `0x467440` and the minimap fog `0x466C20` while V is viewed.
- **`+0xF8` DisplayTimer** = GameTime at the init (`0x464765`); the resource bar's rates refresh once
  GameTime passes it.
- **The notices**: the queue `*(0x51E68C)` (count `+0x99`, 0x11-byte entries, `+0x9D` the last
  sound's GameTime) and 24 per-kind cooldowns at `0x5086EC + 0x18k`, tested at `0x47FAF0` (a kind is
  refused, text and sound, while GameTime is below it) and written at `0x47FEFD`. **`0x47EE30`** (no
  argument, plain `ret` at `0x47EEDA`; one caller, the level init `0x491898`) empties the queue
  freeing each text, zeroes `+0x9D` (`0x47EEB7`) and the 24 cooldowns.
- **The chat lines**: 30 lines at `main+0x12EF`, 0x48 each, their stamps at `+0x40` (`0x463D5F`), the
  head `main+0x2A3E` and tail `+0x2A40`; the expiry `0x463EF0` (one caller, the step at `0x495683`)
  drops the oldest only once its stamp plus `(main+0x37F23 + 1) · 30` ticks is below GameTime
  (`0x463F15..0x463F32`, `jae` keeps it). The chat panel's draw `0x448C70` (callers `0x44A5C6`,
  `0x44AE00`) drops the oldest too while more lines are queued than the panel holds, its height
  `[+0x19]` over `0x4A50B0`'s result + 2 [INFERRED: the font's line height], the tail wrapping at 30 (`0x448CFA..0x448D2C`). **`0x463C80`** (plain `ret` at `0x463C9C`; the level init
  `0x4917D2`) zeroes the head and tail.
- **The particles** carry their own: an emitter's end `+4` and next emit `+8`, a particle's death
  `+0x30`.
- **The pacing gate `0x495230`** takes the smallest `+0x18` of a type-3 record with live units below
  GameTime; from a lag of 900 ticks (`0x49531F`) it scales the due ticks by max(0.01, (3600 −
  min(lag, 3600)) / 2700).
- The engine's own reset list, the level init `0x4917D0`: chat `0x463C80`, notices `0x47EE30`, a
  fresh particle table `0x471D90` (the teardown `0x471DE0` freed the old), the sight records' count
  in LoadMap, the units `0x4854A0`, the wind's next draw, GameTime, the pacing accumulator
  `+0x38A43`, the meteors `0x437CD0`, every active record's `+0xF0`/`+0xF4`/`+0xF8` (`0x464990`), the
  game end's bits; the load wait adds the countdown −1, DeltaTime and the lag counter.

**The engine calls a seek makes, and what each does besides its purpose.**

- **The destructor `0x4866D0` with severity 0 and corpse 0** (C4's `tagpu_tx_replay_death`, mode 0)
  runs every step of [its body](memory.md#0x4866d0-the-unit-destructor-inferred-name-units_destroy)
  — the temporary sight record, the objective's death listeners, the order, reply and projectile
  purges, a carried unit's detach, the grid's clear and the sight removal, the free — but **no
  statistics, no `Killed` script, no explosion and no corpse** (the kind byte 0 jumps `0x48688C` →
  `0x486A98` → `0x486C9E`; `rec+9` = 0 skips `0x486D24..0x486D50`; `rec+0xA` = 0 skips
  `0x486D55..0x486D6F`). **At the owner's last unit it runs the skirmish elimination**
  `0x47BD70(player)` (`0x486E54`), which writes the message ring and plays "MessageArrived".
- **The create `0x485F50` with the slot given** (argument 8): the init `0x485A40` draws the simulation
  RNG twice (`0x485BDE`, `0x485D1B`) and adds the unit to control group 0 (`0x485D29`); `0x485D40`
  makes the COB object and model and **runs `Create`'s first slice at once** (`0x485DE6`), whose
  `emit-sfx` and `EXPLODE` can add particles, explosions and flying pieces (`0x421700` draws the
  simulation RNG up to eight times); the stamp `0x47CC30` (`0x48610F`) pushes the unit at the front
  of its position's bucket; the sight stamp (`0x486178`); a finished unit with def `+0x241` bit 18
  starts `Activate` and queues the reply's speech for the viewed player's units.
- **The control-group setter `0x480250(unit, g)`** (stdcall, `ret 8` at `0x4803B4` and `0x480455`):
  when `+0xAC` is not −1 it removes the unit from the vector `[owner+0x78] + 0x20·(+0xAC)`, then, for
  g ≠ −1, appends it to vector g, and **stores `+0xAC = g` on every path** (`0x4803A9`, `0x48044C`);
  it indexes the table with g and with `+0xAC` unchecked (`0x48026F`, `0x4802D7`). The table is
  allocated per player by `0x480190` (`ret 4`; 0x140 bytes, "SQUADS", `0x480197..0x4801DE`): 10
  entries of 0x20, each with its vector's begin and end at `+0x14` and `+0x18`. A unit's `+0xAC`
  names the vector that holds it, and the destructor's removal relies on it: a raw write of `+0xAC`
  leaves the unit in vector 0, where the create put it, and its death then leaves a pointer to the
  dead unit there. Callers: the create, the destructor, the saved game's restore (`0x487270`), the
  AI (`0x4075D4`, `0x4076D9`, `0x4077B2`, `0x4088FA`) and `0x48D980`.
- **The sort buckets** `[main+0x1429F]`, 10 bytes each: LoadMap computes the columns and rows
  (`main+0x142A3`, `+0x142A7`), the count `(cols·rows + 7) & ~7` (stored at `0x482CBA`, `main+0x142AB`)
  and allocates count × 10 bytes (`0x482CC5`); the off-map bucket is `[main+0x142B7]`. A bucket's
  `+0`, `+1`, `+2` are terrain-derived at load and never written in play (`+1`, the 3 × 3 maximum
  height, is read by the air order payload `0x44E4E3` and the type-3 air proxy's step `0x490763`);
  `+6` heads the chain through the units' `+0x8E`. The functions: the unlink **`0x47CB00`**
  (thiscall on the bucket, `ret 4`: walks the chain from `+6` to the unit with no NULL test, splices
  it out, nulls its `+0x8E`; one caller, the attach `0x48AC5E`); the push **`0x47CB40`** (thiscall,
  `ret 4`: at the front, no NULL test; one caller, the detach `0x48ACDC`, which re-links by the
  carried unit's stale `+0x82`); the move **`0x47CB60(unit, bucket)`** (stdcall, `ret 8`: nothing when
  `+0x82` is already the bucket, `0x47CB6F`; for a carried unit only `+0x82`, `0x47CB79` →
  `0x47CBB9`; otherwise the unlink when `+0x82` is not NULL, a push at the front and `+0x82 = bucket`;
  **no caller and no stored address in the image**, so it cannot reorder a bucket by itself; Impure's
  seek calls it in R9, in a skirmish recording after taking each unit out of its bucket, so every
  call pushes, and on a multiplayer level only for a unit the host does not carry whose `+0x82` is
  not the bucket its owner's image names, which leaves the order the host's); and
  **`0x47CBD0(unit)`** (`ret 4`, one caller, the destructor `0x48682D`): the clear `0x47D0E0`, the
  unlink unless carried, `+0x82 = 0`.
- **The features' model instances** are made by the spawn through
  [`0x45A8D0`](memory.md#0x45a8d0-0x45a950-the-object3do-builders) (`0x423EC2`, stored at record
  `+0x04`) and freed by the destroy or by the teardown's purge `0x422170`, through
  [`FreeObjectState`](memory.md#0x45aaa0-freeobjectstate-the-object3do-destructor) — the purge is not
  callable mid-level, and `0x45AAA0` is `tagpu_reclaim`'s detour.
- **The effects.** The particle teardown `0x471DE0` (one caller, the cascade `0x491B9A`) destroys
  every object of each of the ten layers through its own `[vt+0](../1)` and shifts the vector
  (`0x471E0B..0x471E5A`), then frees the vectors and the table; **no engine function empties the
  layers in place**, and `0x471D90`, the load's allocator, would replace the table. Nothing outside
  the layer vectors points at a particle, and no explosion (0x54) or debris (0x34) record holds a
  unit pointer; a flying piece's block holds its unit at `+0` (`0x42162B`), which only Impure's frame
  packet reads, bounded as a slot. The effects' level init `0x420620` (one caller, `0x4919D2`) also
  re-allocates the explosions' sequence tables and the pieces' ring, so it is not callable mid-level
  either; the ring stays consistent when slots are zeroed (an eviction nulls an owner slot only
  while it still points at the block, `0x437A96..0x437A9D`, `0x437AC8..0x437ACF`).
- **The sounds** test `main+0x37F0C`, `main+0x37F19 & 7` and the NoDirectSound flag `0x51E690` before
  playing, except under the UseWindowsSound backend (`0x51E694`), which branches away first; a seek
  silences nothing by flag — it plays no sound because the kills make no explosion and the
  elimination is gated.

**The unit fields playback leaves to the host's engine.** Playback writes a unit's record from the
image, less the bytes the host derives from the same history as the recording machine. A seek cuts
that history, and so does a create in a slot whose last tenant was the host's own, so the replay
sets these from the recording there (the slice note's *R8b* and *the units' bookkeeping*).
[DISASSEMBLED unless marked]

- **The water state `+0x10A`**, a dword read and stored whole by `0x43DB50` (`0x43DB6B`, `0x43DBED`),
  its only reader and writer (every `+0x10a]` operand in the text was read: the other 17 are a weapon
  def's, below). `0x43DB50` takes the unit as its one stack argument (`[esp+0x10]` after three
  pushes, `0x43DB58`) and returns with `ret 4` (`0x43DBF7`). Its one caller passes the mover in
  `ecx` as well (`0x43DD62..0x43DD65`: `push edi; mov ecx,esi; call`), and it overwrites `ecx`
  before reading it (`xor ecx,ecx`, `0x43DB5C`), so the thiscall and the stdcall shapes are one
  call. Only for walk class 1 or 2 (`+0x110 & 3`) does it compare the height `+0x70` with the sea
  level byte `main+0x1427F`; any other class gets 0 (`0x43DB75..0x43DB86`). Above the sea it gives
  4; within 5 below it, 1; `def+0x22C` plus the height at the sea, 2; `def+0x170` plus the height
  below it, 3 (`0x43DB88..0x43DBC7`, each later test overriding). When no test fires the old value
  stays (the band `0x43DB78..0x43DBCC`), so the value is history. On a change it starts the unit's
  COB `setSFXoccupy` with the new value through `0x4B0A70` (`0x43DBD0..0x43DBE8`; the name at
  `.data 0x505248`). Its one caller is the movement pass `0x43DD20` (`0x43DD65`), called from the
  unit tick (`0x48AFAA`, inside the owner-type-1/2 block, so never for a puppet host's seats) and
  from the `0x2C` receiver `0x48B920` (`0x48BA46`, network games only). **The create stores no byte
  of `+0x10A..+0x10D`** (every store in `0x485A40..0x4861BA` read), so a unit starts with the dword
  its slot's last tenant left and keeps it while it stays in the band.
- **A weapon def's `+0x10A..+0x10E`**: a byte the packets carry, which looks like the weapon's ID
  [INFERRED from that use: `0x49AFCB` compares a projectile's weapon's with a packet byte, `0x49D7E5`
  and `0x49DAD8` store it in the `0x0D` packet, `0x40954D` reads it through the unit def's weapon at
  `+0x1EE`], then rendertype, color, color2 and soundtrigger (the TDF parser `0x42E74C..0x42E788`).
  Its one write, `0x487628`, loops over a unit's three weapon slots (stride `0x1C`) and writes each
  slot's def. Six readers (`0x409682`, `0x409940`, `0x49DCAB`, `0x49DE6D`, `0x49DFA9`, `0x49E0C2`)
  were classified as weapon reads by that pattern, not traced one by one [INFERRED].
- **The state byte `+0x10E`**: the create sets only bit 0, when `def+0x241` bit 18 is set, through
  the setter (`0x48612F..0x486148`); every other change comes from the setter `0x48B090(mask, on)`
  (thiscall, `ret 8`), which starts the unit's Activate (`0x48B101`), Deactivate (`0x48B126`),
  StartBuilding (`0x48B149`) and StopBuilding (`0x48B164`) and sends at `0x48B1F3`.
- **The sight stamp's fields** `+0x7A`, `+0x7C` (i16) and `+0xF8` (u8) (the walk itself: *The sight
  emitter*, under *Line of sight at the map's edge* below). Under LosType bit 1
  the engine keeps one invariant: in the ray fan a unit's byte is non-zero exactly when its fields
  describe a stamp it made, and in the circle its fields always do. The create's stamp `0x482AC0`
  (`0x486178`, `0x486312`) has two branches: in the ray fan it zeroes the byte (`0x482B44`) and
  emits through `0x4825B0` (`0x482B5E`); in the circle it stores the fields and stamps through
  `0x482270`, then `0x481930` (`0x482B6C..0x482C08`), with no early return and no removal. Without
  bit 1 it writes nothing (`0x482B3E` → `0x482C0D`). Every record the ray-fan emitter `0x4825B0` is
  given has y raised to the sea level plus one by a signed compare, so the height it stores inside
  the grid is at least 1 (`0x48266E`). Its five callers: the sight rebuild `0x4816A0` (`0x481836`;
  the raise `0x481800..0x481804`), the move wrapper `0x4827B0` (`0x482824`; `0x482817..0x48281B`),
  the create's `0x482AC0` (`0x482B5E`; `0x482B2A..0x482B2E`), the temporary-record maker `0x482910`
  (`0x4829F4`; `0x4829AC..0x4829B0`) and `0x482830` (`0x482868`), which nothing calls. The removal's
  builder **`0x482090(unit)`** (stdcall `ret 4`; one caller, the destructor at `0x486845`) raises
  the same way (`0x4820F7..0x4820FB`) and builds the record exactly as `0x4827B0` does
  (`0x482093..0x4820FB` ≡ `0x4827B3..0x48281B`), then calls the removal `0x481D50`. Its `+4` and
  `+0xC` are the **addresses** of `+0x7A` and `+0xF8` (`lea` at `0x48209D`, `0x4820BA`). The stamp
  **`0x482270(rec)`** (stdcall, `ret 4` at `0x48259E`) and the removal read only the record's `+0`,
  `+4`, `+8` and `+0xC` (the owner, the stored words, the def's `+0x202` and `+0x170`, the stored
  byte), so a removal walks exactly the cells its stamp walked. The stamp writes only the owner's
  counters (`0x4823A0`, `0x48247F`, `0x482587`) and, for the viewed player's units, clears LosType
  bit 3 and sets `main+0x142F1` bit 2 (`0x482283..0x4822A1`); it sets no explored bit. In the circle
  both look the radius up through `0x4B7F30`, which reads the frame table's count at `0x4B7F41`
  before its NULL test of the table at `0x4B7F48..0x4B7F4A` and answers NULL for an index at or past
  the count; its two sight callers, the stamp (`0x4824E8`) and the removal (`0x481FC8`), dereference
  the answer untested (`0x4824ED..0x4824F1`, `0x481FCD..0x481FD1`).
- **The players phase emits every live unit's sight, every step** (`0x46503C..0x465063`): for each
  record with `+0x00` non-NULL, `+0x73` 1–3 and `+0x146` ≠ 10 it walks the units `[+0x67, +0x6B]` at
  stride `0x118` and calls `0x4827B0` (`0x465053`) for each one with `+0x110` bit 28. The move
  wrapper's other callers: the set-position **`0x48A9F0`** at `0x48AAA0`, only on a change of cell
  or layer — with the footprint cell `+0x76/+0x78` and the layer `+0x110 & 3` unchanged
  (`0x48AA35..0x48AA56`) it stores the position alone (`0x48AA58..0x48AA68`), otherwise it clears
  through `0x47D0E0`, stores, claims through `0x47CC30` and emits — and both paths set `+0x110` bit
  16 (`0x48AAA5..0x48AAB1`); `0x43DA59`, in the movement pass's placement `0x43D6D0`, behind the
  owner-type-1/2 gate, for an uncarried unit whose cell or layer changed and whose move was not
  refused; `0x48B6BF`, in the `0x2C` round robin `0x48B3F0`, network games only; and `0x464DBF`, in
  `0x464DA0`, a copy of the players phase's loop that nothing references (rel32 calls and jumps,
  rel8 and `0F 8x` branches and stored dwords all scanned).
- **A built order node's target.** The constructor `0x43A0C0` sets the reference through `0x4895C0`
  (`0x43A0E3`), copies the action descriptor's flags into `+0x42` (`0x43A18F..0x43A196`) and, when
  they lack bit 9 (`0x200`), clears the reference again with `0x489690(NULL)` (`0x43A1B6..0x43A1C1`);
  the handlers set such a node's target afterwards, through `0x489690`. MEASURED: action `0x19`'s
  node in the stock fixture names a nanoframe with bit 9 clear.

**Who makes a unit, and in which phase of a step** (facts batch F; every caller of the create
`0x485F50` found by a byte scan for rel32 calls and jumps and stored dwords: eleven calls, no jump, no
stored pointer). A step is `0x495490`'s iteration: GameTime, the pump, the capture point `0x4954ED`,
the players phase `0x49555F`, the rest; then the frame callback `0x496790` runs the keys `0x495E90`
(`0x496971`), the draw call `0x4969CD` and Impure's scenario applier at `0x4969D2`, and IdleTick's GUI
dispatch `0x4A9FD0` (`0x499992`) runs between frames.

| Call site | In | Phase |
|---|---|---|
| `0x4028EA`, `0x403D5B`, `0x405104`, `0x41409B` | the order handlers BuildingBuild `0x402640`, MobileBuild `0x403A20`, Resurrect `0x404DB0`, VTOL_MobileBuild `0x413D80` | the unit tick, through the order controllers `0x43B7C0`/`0x43BAD0` (`0x48AF98`, `0x48AF9E`), inside the owner-type-1/2 block; the order destructor's dispatch `0x43A21A` cannot reach a create |
| `0x488700` | GiveUnit `0x488570`, when the receiver's `+0x73` is 1 or 2 (`0x4886B3..0x4886C5`) | from Capture `0x404270` (`0x4046C5`, the unit tick); from the pump's `0x14` case (`0x45577B`, network only); **from the SHARE dialog**: its handler `0x4934B0` (stored at `0x493759`; the gadget SHARUNIT, `0x493663..0x493673`) → `0x4933E0` → `0x493486`, in IdleTick's GUI dispatch, between steps |
| `0x41794F` | the console's fallback `0x417890` (pushed at `0x4195DF`), which makes a unit by its typed name | a typed command between steps; run only at level 4 (`0x4B79DD..0x4B79F5`), which the keys and the chat pass only under SoftwareDebugMode bit 2, `main+0x37F2F` (`0x496380..0x496393`) |
| `0x4653D9` | the Deathmatch respawn, in the players phase | **after its owner's emit loop**, then `0x4816A0(1)` (`0x4654EF`); needs ActiveCommanderDeath 2, which the skirmish screen cannot set, and is skipped for a watcher (`0x465154`) |
| `0x48718E`, `0x488462`, `0x497002`, `0x4977BB` | the saved game's unit restore, the campaign's placed units, the seat setup, the loader body | the loader |

The unit init `0x485A40` has three callers: the create (`0x4860A0`), CreateFromNetwork `0x4861D0`
(`0x4862B8`; its own callers `0x4553E4`, `0x48B497`, `0x48BA00` are reached only through the pump,
which returns without `main+0x2A44` bit 0, `0x453D4B..0x453D5C`) and `0x485EF8`, in `0x485E90`, which
nothing references. So **in a skirmish a unit is made after the players phase of a step only by the
SHARE give, the console's fallback under SoftwareDebugMode bit 2, and Impure's scenario applier**;
over direct calls, the rest of the tick after `0x49555F` reaches no create. Not established: whether a
stock skirmish's SHARE dialog lists an AI ally (the code accepts any active type-1–3 record without
the watcher bit, `0x49357B..0x4935FA`; not run).

**A unit's COB object lives exactly as long as the unit.** `0x485D40` (callers the create `0x4860A6`,
CreateFromNetwork `0x4862BE`, and `0x485EFE` in the unreferenced `0x485E90`) allocates `0x544` bytes
(`0x485D73`), runs the base constructor `0x4B0610`, which zeroes the eight thread records' status
words at stride `0xA4` from `+0x1C` and the running count `+0x53C`, and stores the object at `+0x9A`
(`0x485D9F`); the destructor deletes it through `[vt+0x50](../1)` (`0x486D8A`) and nulls `+0x9A`
(`0x486D8D`). No thread record outlives its unit, so a seek's kills and creates leak none.

**A unit's control group across its death and a create.** The destructor calls `0x480250(unit, −1)`
(`0x4867AC..0x4867AF`), so a slot whose unit died holds `+0xAC` = −1. The unit init calls
`0x480250(unit, 0)` (`0x485D20..0x485D29`, `ebx` 0): the setter removes the unit from the vector its
old `+0xAC` names only after finding it there (`0x480258..0x4802C2`), then appends it to group 0 and
stores `+0xAC` = 0, so every create starts in group 0 whatever its slot held. **The destructor's
elimination**: at its owner's last unit it makes two game-type tests (`0x486E12..0x486E54`), the
network elimination `0x450380` for type 3 (`0x486E32`) and the skirmish one `0x47BD70` for type 2
(`0x486E54`).

**The radar pass `0x467440` and the minimap's fog composite `0x466C20`**, the viewed record's 30-tick
pass (`0x46556D`, `0x465572`, in the viewed record's iteration of the players phase, `cmp
bl,[main+0x2A43]` at `0x465565`, behind the deadline `0x46507D..0x465092`). The players phase is
called at `0x49555F`, after the unit tick's call `0x4954ED`, so on a puppet host the pass of step k
sees the replay's step k. **No image reproduces a pass.** The pass of step P reads the units the
unit tick of P has just moved — image P+1's — but every global as it stands mid-step, and a write
later in step P reaches image P+1 and not the pass. MEASURED 2026-10-09 on the tenseat fixture,
whose LOS command is typed at GameTime 900: image 901 carries LosType 6 with the bit 8 a LosType-4
pass set on every enemy unit (the sight loop's test of LosType bit 1, `0x46775D`), the bits stay
through image 930, and the pass of step 930 clears them on the 19 units the viewed record's sight
counters do not cover. **A typed LOS command does not rerun the pass**: LOS `0x416D50` writes
LosType (`0x416D68`), calls the settings save `0x430F00` and then the sight rebuild `0x4816A0(0)`
(`0x416D76`), which ends in the fog composite and the radar rebuild (`0x48191A`, `0x48191F`) and
writes no `+0x110` on its path (its row above). The order on the recording machine is step 900,
whose players phase runs the pass under LosType 4, then the typed command, then GameTime 901
(`0x4954C0`) and the capture `0x4954ED`. The demo recorder's seek and takeover therefore run the
pass on T's world and then, when the host views the recording's viewed record, take bits 8–10 from
the recording (`tagpu_replay.c`, R14b; the skirmish slice, O14). In that view a playing takes bits
8–10 from each image after its step as well (THE RECORDED VIEW'S CONTACTS, `contacts_put`), because
the host's pass of a pass step also sees the units the recording machine made after its own pass (the
radar pass's row above), and in multiplayer tests each remote unit at its owner's position, which a
sight cell's edge can put in the next cell (MEASURED 2026-10-23: a four-seat recording's slot 1510,
0.21 units from its owner's image at z 1417.87 against 1418.08, row 42 on the recording machine and
43 on the host). In a takeover those bits are the
fork's AI input: the acquisition records it makes start with `+0xED` 0, so each seat's first
`0x40AA40` runs in iteration T's players phase, before the next pass, and reads them (`0x40AB3F`).

- **The radar pass** (no argument, plain `ret` at `0x467836`) runs only when `main+0x2A3C` ≥ 2
  (`0x46744D`). Its first loop clears bit 12 of every live unit (`0x4674AE`) and sets bits 8–9 in
  three cases — the owner is the viewed player (`0x4674B4`); the owner is allied and its PlayerInfo
  has `+0x97` bit 6 (`0x4674C5..0x4674D9`); the viewed record's PlayerInfo has the watcher bit,
  `+0x9B` bit 6 (`0x4674DB..0x4674EB`), so in a watcher's view every live unit — and clears bits
  8–10 otherwise (`0x4674EF`). Its later loops add the viewed player's radar picture through the
  unit walk `0x47E890` (a visitor's first virtual method per unit, the list through `+0x8E`), whose
  three visitors `0x467840`, `0x467960`, `0x467980` (vtable slots `0x4FD55C`, `0x4FD558`,
  `0x4FD554`) store `+0x110` alone; its fourth loop writes `+0xB0` and bit 12 only for a type-1 or
  type-2 owner; its fifth, the sight loop, sets bit 8 (`0x46780E`) from the viewed record's sight
  counters, which under true LOS hold the live temporary sight records' stamps as well. **It writes nothing in `main+0x142F1`**: the `or byte [..+0x142F1],2` at `0x46742F`,
  just before it, is the last instruction of the radar rebuild `0x466DC0` (`ret` at
  `0x46743D`), which the players phase calls for the viewed record every step (`0x465072`) and the
  sight rebuild after its composite (`0x48191F`).
- **The composite** (no argument, plain `ret` at `0x466DB1`) runs only when `main+0x142F1` bit 2
  asks, and clears the bit (`0x466C29..0x466C44`, the one instruction that clears it). It rebuilds
  the fogged image `+0x142DF` from the unshaded base `+0x142E3`, the explored map and the viewed
  record's counters (`0x466D01..0x466D44`): an entry without the viewed bit gets the unexplored
  colour, an explored entry whose counter is 0 the shaded one, any other the terrain's. It reads the
  counters `+0x7C` with no NULL test (`0x466D22..0x466D25`). Then it sets bit 1, DrawMinimap's
  redraw flag (`0x466DA4`). Its two callers are the pass (`0x465572`) and the sight rebuild just
  after `0x4816A0` sets the bit itself (`0x481905`, `0x48191A`).
- **`main+0x142F1`**: bit 2 has five setters, each an `or ...,4` — the stamp `0x482270` (`0x4822A1`),
  the explored-map setter `0x481930` (`0x481D3B`) and the removal `0x481D50` (`0x481D81`), each for
  the viewed player's units only, the minimap set-up (`0x466A6E`) and the sight rebuild
  (`0x481905`). A player who owns no unit, as a watcher, therefore never asks after the last
  rebuild. Bit 1 is set by the composite (`0x466DA4`), the radar rebuild (`0x46742F`) and the camera
  module's eye writers, and cleared by DrawMinimap `0x466B00` (`0x466B16`; its one caller
  `0x46961F`); bit 0 is the radar blink's toggle (`0x4665A3..0x4665BB`). Every reader of the word is
  minimap code: display state.
- **The images' lifetime**: made by the minimap set-up `0x4669B0` (one caller, the level init's
  `0x4919C3`), freed by `0x466AA0` (one caller, the teardown's `0x491BAE`); nothing reads them off
  the game thread (the frame packet copies them in the in-play draw's `after`).

**The sight counters' lifetime.** The player init `0x464700` (one caller, `0x4649B1`) allocates
`[rec+0x88]` bytes and stores the block at `+0x7C` with its length (`0x46486F..0x46487B`); the
level's teardown `0x464A00` (one caller, `0x491BA9`) frees every record's block and stores NULL and 0
(`0x464A24..0x464A38`). So a record outside a level reads as NULL/0, and nothing frees the block
while the game thread reads it.

**The level start `0x465E30` and a watcher's resources.** Called from the loader body (`0x497ADE`,
`0x497C15`), it sets each record's energy `+0x8C` and metal `+0x98`, unless `main+0x38D6B` is set
(`0x465E56..0x465E5E`), by game type (`0x435100`): type 1, GameingState's own table
(`0x465F22..0x465F6C`); type 2, the skirmish, `(float)` of the settings' ints at `settings + 0x18·i +
0x10` and `+ 0xC` (`0x465EF5..0x465F1A`, `settings` = `*(main+0x29A0)`); type 3, 100 × the
PlayerInfo words `+0xA1` and `+0xA3` (`0x465E80..0x465EED`). The SKIRMISH screen does not fill a
watcher's entry: MEASURED 1365/16385 and 11/10 in V's pair in three processes. The economy `0x401360`
runs only for a type-1 or type-2 record (`0x465539..0x46555A`), so nothing changes a type-3 record's
pair afterwards. **The resource bar shows the viewed record clamped to its storages**: it reads
`main+0x2A43`'s record (`0x468E4B`) and clamps to `+0xA4`/`+0xA8` (`0x468F14..0x468F6C`), which the
player init zeroes (`0x46478F`, `0x464795`); so a watcher's bar shows 0 whatever its record holds.

**The frame visit's site, `0x4969CD`** (`push ebx; push ebx` at `0x4969CB`, `call 0x468CF0`), the
in-play frame callback `0x496790`'s draw call. The pause test `0x496920..0x496926` (`IsGamePaused`
`main+0x38A51`) jumps over only the scheduler `0x495230` and the tick `0x495490`, to `0x496969`, and
every path from there reaches `0x4969CD`, so the site runs on paused frames too. The scenario
applier's tick hook takes the next instruction's bytes (`0x4969D2`), disjoint from these.

**Live rewind's pause guard, `0x496920` [DISASSEMBLED].** The six stolen bytes
`84 98 51 8A 03 00` are `test byte [eax+0x38A51],bl`; `jne 0x496969` at
`0x496926` skips the scheduler call at `0x496928` and the step call at
`0x49693E`. A before-observer sets pause while the game-thread rewind state is
active, then executes that same test. This orders the pause before every simulation
entry through this frame path, even when input has toggled it since the previous
draw. The later draw observer alone cannot provide that ordering. The preceding
`main+0x37EBE` test at `0x496918` may branch directly to `0x49697B`, bypassing
both this test and simulation. [Live rewind](../tadr-port/demo-recorder-s4-rewind.md)
records the pause-key measurements. **The hold keeper** is the same observer: while a pause the
DLL's test pacing took is held (THE PAUSE AT T, or a test launch's hold, below), it sets bit 0
before the test as rewind does, so a Pause key (`0x4960AA..0x4960BC` toggles the bit) or the
in-game menu's OK clears it for nothing; `resume` forgets the pause first. MEASURED 2026-10-09
(`resource-mex` held at GameTime 1): after the Pause key, and after ARMOPT opened and its OK
clicked, two peeks each read GameTime 1 with the bit set; after `resume` the level ran on.

**What the paused path keeps, and the arrows on a held level** [DISASSEMBLED 2026-10-09]. The
scroll poll `0x41CE90` (called at `0x496976`, on paused frames too unless `main+0x37EBE` bit 0, an
in-game GUI screen, is set: `0x496969`/`0x49696F`) steps the camera by the byte `main+0x1434D`
(ScrollSpeed) times the dword `main+0x38A3F` (`0x41CE9B..0x41CEA2`), capped at `0x80`, and skips
the step when it is 0 (`0x41CEBA`). `main+0x38A3F` is the 30-Hz clock's ticks since the
scheduler's previous call (`0x495243..0x495249`, the clock `0x4B6340`), which a paused frame does
not call (`0x496926`), so a held level keeps the last running frame's value: a frame shorter than
a 30-Hz tick leaves 0, and then no arrow moves the camera. MEASURED 2026-10-09 (c2-weapon-keys,
held at 301 after a run at the turbo's k = 1): `main+0x38A3F` read 0 and an arrow held two seconds
moved nothing; at the game's own pacing it read 1 and the arrow scrolled. A level held at a
launch's first step keeps the load's value and scrolls. The page that a selected unit's build
menu opens on comes from the same frame callback: `0x4969A4` tests bit 4 of `main+0x37EBE` (set
by the selection, `0x41C110`), `0x4969A8` clears it and `0x4969B4` calls the page router `0x41B2E0`
(*The left sidebar: its pages, its buttons and who takes a press*), so the page is up by the frame
after the selecting click.


**Corrections to this map's earlier lines**, each re-read here: `0x416390` (`call 0x424840; ret 4`)
is the console command **BurnAll** and `0x4163A0`, which destroys the feature under the cursor
with `force` 1 (`0x4163C8`), is **BurnOne** — the command table's entries at `0x50203C` and
`0x502048` (name, handler, 4), stored at file offsets `0x100640` and `0x10064C`; neither is a debug
key. And a nearest-preceding-call-target attribution places two of the game end's sites wrongly:
`0x464E55` is in the dialog handler `0x464DE0`, not in `0x464C60`, and `0x4995FC` is in the in-play
handler `0x499200`, not in `0x499100` (whose last `ret 4` is `0x4991FB`).

**MEASURED 2026-10-01** (Wine, scripted runs on a private X server, the slice note's *S3b, verified
and measured*): the pool's written extent reached 242 of 3000 records in the 1,432-unit battle, 175
in the naval fixture and 5 in the forest fire; with Mapping on (LosType 5) the explored map changed
in 226 of 3,810 iterations, 5,669 bytes in their runs; with it off it never changed in any
fixture.

### The demo recorder's record frames: the engine facts they predict from [DISASSEMBLED + MEASURED 2026-10-10]

Recording format 25 makes the bodies of a stream block's cells, order lists, events and state
records from four *record frames* (`tagpu/ddraw/src/tagpu_recframe_cells.c`, `_orders.c`,
`_events.c`, `_state.c`; the bytes in the skirmish slice's
[*The file*](../tadr-port/demo-recorder-skirmish-slice.md#the-file), the measurements in
[making recordings smaller](../tadr-port/demo-recorder-size.md#l3-the-record-frames-measured)). Every
fact below is a predictor's, used as a size rule only: a value the prediction misses is coded as a
residual or raw, and the writer writes a block in the frames only when a second decoder gives its
records back, so a wrong fact costs size, never data. Each address was re-read in
`pristine/TotalA.exe.pristine` on 2026-10-10 (`i686-w64-mingw32-objdump -d -M intel`).

- **The simulation RNG's step, `0x4B6C30(n)`** (`stdcall`, `ret 4`), which the state frame's
  Park–Miller method imitates. `n` < 2, compared signed (`cmp edi,2; jge` at `0x4B6C35..0x4B6C38`),
  returns 0 without touching the seed (`0x4B6C3A..0x4B6C3D`). Otherwise it loads the seed
  `[0x51FC88]` (`0x4B6C41`), forms 16807·s by shifts and `lea`s (`0x4B6C4C..0x4B6C75`, 7 · 5 · 5
  · 16 + 1 = 2801, · 3 · 2 + 1 = 16807) and subtracts `q · (2^31 − 1)`, `q` = s / 127,773 by the
  multiply-high with `0x69C16BD` (`0x4B6C47..0x4B6C60`): Schrage's form of 16807·s mod 2^31 − 1.
  A result ≤ 0 gets `+ 0x7FFFFFFF` (`test; jg; add` at `0x4B6C7C..0x4B6C80`), the seed is stored
  at `0x4B6C87`, and the draw is the new seed mod `n` (`div edi` at `0x4B6C8C`, the remainder
  returned). Checked numerically: for every seed in 1..2^31 − 1 tried (2,000 consecutive and
  200,000 random), the instruction sequence gives 16807·s mod (2^31 − 1), 0 becoming 2^31 − 1 —
  the frame's `f`. The seed's other store is the level's seeding (*The sim RNG is per machine*,
  above). MEASURED by the L3 research on the format-9 battle: the seed moves a median of 16 steps
  an iteration, at most 12,622, which is why the frame codes it as a step count.
- **The feature tick's cursor `main+0x14257`** (*The feature tick `0x424050`*, step 2, above):
  `dec ecx` at `0x4240A9`, stored at `0x4240AA`; the `jns` at `0x4240BC` tests that decrement's
  sign (the `mov`s between set no flags), and below 0 the cursor becomes `[main+0x14233] ·
  [main+0x14237] − 1`, W · H − 1 (`0x4240BE..0x4240CC`), and that tick skips reproduction (`jmp
  0x4241A8` at `0x4240D2`). The state frame predicts the cursor as `last ? last − 1 : W·H − 1` and
  the pool's three heads as last time; an `RK_FEATURES` the prediction gives exactly, after an
  explicit one since the reset, is a marker
  in the kinds section, with no state-frame bytes (format 18; MEASURED 2026-10-12 on the heavy
  four-player hour: 107,405 of 108,090 iterations, the cursor stepping as the tick steps it and
  the heads unchanged).
- **The wreck pool's links** (*The pool's start*, above): the level load's loop
  `0x421F5F..0x421F7E` stores next `i + 1` at `+0x00` and prev `i − 1` at `+0x02` of every
  `0x30`-byte record, then −1 as record 0's prev (`0x421F8D`) and the last record's next
  (`0x421F94`, `+0x17FD0` in stock). So no record of a level is all zero, and a snapshot that
  stored every record that is not zero stored the whole pool; the snapshots (since format 10) store the
  records that differ from the pool at the end of the segment's first iteration instead (MEASURED
  2026-10-10 on the size battle's recording rewritten as format 10: 5,524 records in its 8
  snapshots, against 65,536).
- **A shot's start and its units** (*What a shot's packet says of its start*, above): `0x49D9C0`
  fills the packet's `+0x01..+0x0C` from the shooter's `+0x6A..+0x75` (`lea eax,[edi+0x6A]` at
  `0x49DA9F`; x and y stored by `0x49DAB6`, z read at `0x49DABD` and stored at `0x49DAC7`) and
  `+0x0D..+0x18` from the target point at `[ebx]`; the receiver `0x49D270` reads the `u16`
  shooter at `+0x21` (`0x49D329..0x49D34A`, 0 for none) and the `u16` target at `+0x1F`
  (`0x49D388..0x49D3B7`), each scaled by `0x118` into `main+0x14357`. The events frame codes a
  shot's start and target point against the shooter's and the target's `+0x6A/+0x6E/+0x72` in
  its unit copy and the target ID against the shooter's. MEASURED by the L3 research: the start
  lies within 63 world units of the shooter and the target point within 15 of the target on
  99.9 % of shots.
- **A death's image** is the unit as the destructor `0x4866D0` found it (*The demo recorder's event
  sites*, above); MEASURED by the L3 research, it differs from the unit copy in 0 to 17 dwords, so
  the events frame codes it as a mask against the copy.
- **An order node's action and issue time** (`0x43A0C0`, the common order constructor, `ret
  0x18`): it stores the action byte at `+0x04` (`mov [esi+0x4],al` at `0x43A0E0`) and GameTime
  `[main+0x38A47]` at `+0x46` (`0x43A11F..0x43A12E`). The order loader `0x43A420` (*The walk*,
  below; its one caller `0x487594`) writes both as well: the same vtables `0x4FD2CC` then
  `0x4FD2C8` (`0x43A432`, `0x43A445`), the load's GameTime at `+0x46` (`0x43A464..0x43A471`,
  `mov [ebp+0x46],ecx`) and the action at `+0x04` (`0x43A60A`), so every node of a list restored
  from a save has the same `+0x46`. The orders frame's scheme 2 (format 22, a stream with
  `RT_HOLD`) matches each node of a re-sent list to the kept node with the same `+0x04` byte and
  `+0x46` dword: under the lossy filter a list loses its done head or gains a node in front, and
  node i no longer meets kept node i. A restored list's nodes of one action match the first such
  kept node after the last match, which costs size only: the decoder reads the index. Re-read
  2026-10-13.
- **A unit's heading `+0x66`** (*the unit frame*, above: the mover adds its turn rate as a word):
  the events frame (format 22) predicts an explicit nano spray's point from the same unit's last
  four, as it was, moved with the unit's `+0x6A/+0x6E/+0x72`, or turned about the unit by the
  change of `+0x66` either way (a 16-bit angle, one turn 65,536), in integers.
- **An order node's timer `0x439E80`** (the handler entry facts of
  [*the takeover from a recording*](#the-takeover-from-a-recording-seat-types-the-ais-objects-passability-and-the-order-nodes-disassembled-2026-10-02)):
  it rewrites a node's `+0x0A` to GameTime + ticks on every call, so a list the handlers re-arm
  is sent again every tick with one node changed; the orders frame codes it against the last
  whole image of the same (slot, list).
- **The cells' repeat is the recorder's, not the engine's**: the recorder's `cells_tick`
  (`tagpu_recorder.c`) writes the feature tick's changed cells as tick input and leaves its shadow
  as it was, so the next iteration's images repeat them. MEASURED by the L3 research on format-9
  recordings, after each run's first iteration: the images equal the previous tick input record
  for record in 7,399 of 7,401 iterations of the battle, 3,956 of 3,985 of Town & Country and
  5,757 of 5,904 of the air war. The cell's fields are *The cell*, above.

### The demo recorder's lossy stream: what a held byte meets on a puppet and at a takeover [DISASSEMBLED 2026-10-11]

A skirmish recording's stream goes through a lossy filter
(`tagpu/ddraw/src/tagpu_reclossy.c`): the bytes `tagpu/ddraw/replay-hold.tsv` names may keep an
earlier value between two refreshes, and at a refresh the stream's world is the writer's exact
copy ([making recordings smaller](../tadr-port/demo-recorder-size.md#l5-the-lossy-plan-measured-offline)).
The table holds, in the unit record, the three weapon slots' target (`+0x0`, `+0x2`), callback
(`+0x4`), aim complete (`+0x8`), aim heading and pitch (`+0x16`, `+0x18`) and flags but bit 1
(`+0x1B` mask `0xFD`), and the ledger's produced, requested, granted and debt of both resources
(`+0xBC`..`+0xC8`, `+0xD4`..`+0xE0`; *The ledger*); in a main list node (`+0x5C`), the state
`+0x05`, the wanted events `+0x06`, the deadline `+0x0A`, the leash `+0x2E`, p3 `+0x3E`, the fired
events `+0x4E`, the owned object `+0x52` and the flags `+0x42` but `0x8`, `0x100`, `0x10000`,
`0x40000`, `0x80000`, `0x200000` and `0x400000`. Each row cites its readers below. From format 15 on, a nano
spray its unit's spray session predicts is left out and every reader makes it again after its
iteration's events (`RK_SPRAY_SESSION`). The filter's
correctness rests on these engine facts, from a census of the readers and writers of every field
the plan would hold (2026-10-11; rel32 and stored-pointer scans of `.text`, each hit disassembled):

- **Every engine write of a held byte on a puppet is overwritten by the playing's next write.**
  The playing writes every live unit's whole image -- each byte `replay-layout.tsv` leaves a
  value, so every held byte -- and every node of both lists of every live unit at every iteration
  (`tagpu_replay.c`, `units_put` through `unit_put`, and `orders_put`), so what a held byte needs
  is that no reader shows it between two of those writes. The engine writes of held bytes known to
  run on a puppet: the unit destructor `0x486760`, at a replayed death of kind 5 with a killer,
  read-modify-writes the killer's `+0xD4` with `(1.0 − victim +0x104) × def +0x18A`
  (`0x486CAD..0x486D1E`, the credit above); the death walk `0x489740` ORs event 8 into node
  `+0x4E` (*The pointer fields*, `+0xA2`); the shot receiver `0x49D270` writes the named slot's
  pitch `+0x18` (`0x49D37C`) and heading `+0x16` (`0x49D384`) from the message; the builder
  `0x49CC20`, called from the receiver at `0x49D479`, zeroes the slot's aim complete `+0x8`
  (`0x49CCA2`, `ebx` 0 from `0x49CC2F`, `edi` the slot from `0x49CC75`); and the create's
  `0x489800` call (`0x485C58`, after `0x48A160` at `0x485C50`) writes a fresh unit's slot targets
  and flags bit 4. No reader of them runs on a puppet in between: the bottom panel reads the
  ledger's `+0xCC`, `+0xD0`, `+0xE4` and `+0xE8`, none held; the shot builders read the heading
  and pitch the receiver has just written; and `+0x4E`, the aim and the targets are read by the
  order controller, the handlers and AutoAim, which the unit tick runs only for a type-1/2 owner
  (`0x48ADC9..0x48ADD7`, `0x48AEB5..0x48AECD`). The Deathmatch respawn's writes of `+0xBC`/`+0xD4`
  (`0x465457..0x4654E7`) sit behind the defeat test the seat fill disarms on a puppet [INFERRED
  that they do not run there]. All DISASSEMBLED.
- **The node destructor's reads that the puppet rows neutralise.** `0x43A1F0` reads node `+0x06`
  (`0x43A1F5`), tests bit 1 (`0x43A1FE`) and, when it is set, calls the order's handler with event
  2 (`0x43A21A`), tests `+0x42` bit `0x10000`
  before its `0x489800` call (`0x43A2AB`), and deletes the owned object when `+0x52` is set
  (`0x43A264..0x43A2A2`). It runs on a puppet (from `0x439EB0`, `0x439F80` and Impure's order
  teardown); the playing's `ord_values` forces `replay-layout.tsv`'s `puppet0`/`puppet1` rows on
  every node it writes, after the image, so a held value of these bytes never reaches it.
- **Owner code dereferences a slot's target unbounded.** The resolver `0x48A190` and AutoAim's read
  `0x48A1E0` (*The weapon slots*, above). Their callers are handlers, the AI, the retaliation
  (`0x407141`, behind the damage apply `0x489CE0` that a puppet refuses) and AutoAim, whose one
  caller `0x48ADDA` sits behind the owner's type-1/2 test (`0x48ADC9..0x48ADD7`), so none runs on a
  puppet; all of them run after a takeover. A held target is a value the engine produced for that
  unit, so its index is a slot of the unit array: what the missing tests let through is a target
  that has died or whose slot has been reused since, not an address outside the array. So a
  takeover starts only where the stream holds no target at an earlier value: at a refresh of a
  segment that holds, at any iteration of one that holds nothing (`tagpu_replay.c`, `tk_bound`,
  `recr_exact_at`). There a skirmish's targets are the engine's own; a multiplayer segment's
  world is exact against the composed parent, whose remote images carry their owners' machines'
  targets (the composition's residuals: [multiplayer slice](../tadr-port/demo-recorder-multiplayer-slice.md#the-takeover-of-a-multiplayer-recording)).
  The units the image does not describe have their three slots' targets set to none, (0,
  `0x8000`), the words AutoAim writes when it drops a target (`0x48A2AF`, `0x48A2B4`, DIS)
  (`tk_orphans`).
- **The shot builder reads slot `+0x10` on a puppet** (`0x49CE62`, *The weapon slots*): a field
  constant after the create, so the plan's "read by owner code only" does not hold for it, and the
  table does not hold it.
- **The shot builders read a slot's aim heading and pitch on a puppet only after the receiver
  writes them.** `0x49CDE0` (`0x49CE4F..0x49CEBC`), `0x49C9C0` and `0x49CC20` run only inside the
  shot receiver `0x49D270` (`0x49D44E`), which first writes `+0x18` (`0x49D37C`) and `+0x16`
  (`0x49D384`) from the message on the slot `msg+0x23` names, so a puppet's shot reads the
  message's values whatever the image held. The flags' bits 2–3, which the builders read at
  `0x49CB7D`, `0x49CD38` and `0x49CF5C`, are written once at init (`0x49E094..0x49E09F`), and
  Impure's splices there take the index from the slot's address.
- **A spray session's implied sprays are exact, and only their order moves.** An implied spray is
  the unit's own spray of 1, 2 or 4 GameTimes before, byte for byte, or with its point moved by
  exactly the unit's position change since (`+0x6A..+0x75`, never held), and the filter leaves a
  spray out only when that prediction equals it, so a puppet calls the emitter `0x4720D0` or
  `0x472200` with the recorded point, box and layer in the recorded step. What moves is the
  spray's place among its step's events, after them, units ascending: an emission changes no
  simulation state, and the order of a step's emissions decides only the particles' order in the
  layer (the draw order and the order of the front-destroy past the layer's cap) and, when the
  shared pool `0x51E610` runs out within the step, which emission makes no particle (*The nano
  spray*, above). An owner decision of 2026-10-11 (OD5 of the plan); the owner confirmed on
  2026-10-12 that it covers the pool.
- **`0x408198` is AI code, not a handler.** `0x408100`, which reads the head node's `+0x42` bit
  `0x8` there (`0x408191..0x40819C`), is method 0 of the two-entry vtable `0x4FC9A8` {`0x408100`,
  `0x408600`}, stored by the AI object's constructor `0x408CB0` (`0x408D3D`) in one of its ten
  sub-objects, which the AI tick `0x408C40` calls (`0x408C8E`); `0x408C40` runs only for a record
  with `+0x74` set (`0x46502A..0x465031`), and a puppet seat holds none. The table does not hold
  bit `0x8` all the same: with no reader on a puppet it could be, but that loosening is not taken.
  Nor bit `0x80000`: its reader `0x439D20` reads only the sub list (`unit+0x60`, `0x439D25`),
  which is never held, so on a main list node it could be held too.
- **The local ground movement class reads a held flag of the head node, and never on a puppet.**
  Slot 1 of vtable `0x4FD458`, `0x44F2A0`, the class `0x44F010` the mover's constructor makes at
  `0x43DCF2`, tests the unit's main list head's `+0x42` bit `0x800000` (`0x44F3DF..0x44F3E9`). A
  type-3 owner's mover holds the proxy class `0x44F570` instead (`replay-layout.tsv`'s mover row,
  `0x43DC48..0x43DD08`). Slot 1 is called by the owned-object installs `0x4388D0`, `0x438930`,
  `0x438A00` and `0x438AD0`, which only handlers call, and by the node destructor at `0x43A285`
  with argument 0, which stores it and leaves at `0x44F2D8..0x44F2E3` (to `0x44F41D`) before the
  read. DISASSEMBLED.
- **Not established**: whether `0x406F50`'s callers (`0x499CBC`, `0x49A09B`, `0x49A837`), which set
  `+0xBB` bits, run on puppet impacts; whether the order entries `0x487BF0` (from `0x488514`) and
  `0x4881E0` can reach a puppet's units; callers of `0x439CF0`, `0x43AB90`, `0x43AD50`,
  `0x43AF00`/`0x43AF10`, `0x43B350`, `0x43B3A0`, `0x43B730` and `0x43BBD0` through jump tables
  (none by rel32 or stored pointer).

### The takeover from a recording: seat types, the AI's objects, passability and the order nodes [DISASSEMBLED 2026-10-02]

S4b of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s4b-the-takeover-and-its-fork)
turns a puppet level into a live skirmish at GameTime T inside the seek
([the takeover note](../tadr-port/demo-recorder-s4-takeover.md)). These are the engine functions it calls
and the ones it read to decide what a rebuilt order node may hold. Read from
`pristine/TotalA.exe.pristine` (`objdump -d -M intel`).

**S4c's file lifetime** ([the fork rules](../tadr-port/demo-recorder-s4-forks.md))
adds no engine address or byte patch. The existing teardown entry `0x491B60`
continues to end playback before the cascade; `tagpu_replay_level_end` closes the
reader and queues a last-used touch. The common flip observer in
`tagpu_overlay.c` also polls file requests at menus, whereas seek/takeover retain
the frame visit at `0x4969CD`. The new maintenance worker reads no engine memory
(source checked by `thread-split-check.sh`). These statements follow the existing
hook plumbing in `tagpu_reclaim.c`, `tagpu_overlay.c` and `tagpu_replay.c`, not a
new disassembly inference. Its Windows file-replacement probe and ordering
argument are in the fork note.

**What the takeover calls**, on the game thread, between two steps:

| Function | Convention | What it does |
|---|---|---|
| `0x463C60` the seat-type setter | `thiscall (rec, type)`, `ret 4` | `rec+0x73 = type` (`0x463C67`), and PlayerInfo `[rec+0x27]+0x94 = type` unless the type is 3 (`0x463C64`, `0x463C6A..0x463C6F`) |
| `0x4B4F10` the allocator | `cdecl (size)` | the player init allocates the AI object with it, `0x3D` bytes (`0x4648A7..0x4648AE`) |
| `0x408CB0` the AI object's constructor | `thiscall (obj, rec)`, `ret 4`, returns `obj` | the player init stores its result to `rec+0x74` (`0x4648B5..0x4648C1`); it reads `[rec+0x78]` and points sub-objects 0x20 apart into it (`0x408CF0..0x408D0C`), so the group table `+0x78` must exist first |
| `0x40B320` the acquisition record | `stdcall (seat)`, `ret 4` | allocates `0x10D` bytes, constructs them through `0x409160` and stores the result to `0x5119C0[seat]` (`0x40B340`), then calls `0x409730` on it; on an allocation failure it stores 0 and calls `0x409730` with `ecx` = 0 (`0x40B350..0x40B35B`), the engine's own crash, as at every load. The constructor zeroes the list bounds `+0x09` and `+0x0D` (`0x4091A1`, `0x4091A4`) and calls `0x40A150` at `0x4092E8`, which draws the simulation RNG `0x4B6C30` eight times (`0x40A15F..0x40A23B`). The player init passes `rec+0x146`, the record's map bit (`0x4648C4..0x4648CD`), and the table has ten entries, so a bit of 10 would write the dword at `0x5119E8`. The teardown frees through the same byte (`0x40B390` at `0x464A4F`), and the level's slot setup `0x4854A0` copies it into each of the record's unit slots' `+0xFF` (`0x485908..0x48590E`), the owner byte the radar pass's loop 4 indexes the table with |
| `0x4648E0` the AI profiles | `cdecl (void)`, plain `ret` (`0x46498D`) | ends by calling `0x409F80(i)` and `0x40A040(i)` for each record that is active and of type 2 (`0x46494E..0x464982`) |
| `0x440A40` the passability refresh | `stdcall (xz, wh)`, `ret 8` | calls `0x440830(ctx, xz, wh)` for each of the 32 contexts `0x512358..0x512758` whose first dword is set (`0x440A4B..0x440A67`). `0x440830` sign-extends x, z, w and h from words, computes x+w+1 and z+h+1 in 32 bits, clamps x and z at 0 (signed) and the ends to `ctx+0x10` and `+0x14` (unsigned) (`0x440833..0x440897`), so `(0, 0x7FFF7FFF)` is every context whole |
| `0x48BD00` deselect all | `cdecl (void)`, plain `ret` | `+0x110 &= 0xFFFFFF2F` on every slot from `[main+0x14357]` to `[main+0x1435B]`, then `0x491D70(0)` (`0x48BD00..0x48BD42`) |
| `0x464290` the seat activation | `stdcall (seat, type)` | zeroes `rec+0x74` without freeing it (`0x46434A`), including types 0 and 3; sets the active DWORD to 1 (`0x464364`), so type 0 is not an active-DWORD clear |
| `0x4904B0` the objective's disarm | `thiscall` | writes only `obj+0x88 = 0` (`0x4904B0..0x4904BA`); the takeover writes 1 back |

**The player init's tail, `0x46489D..0x4648CD`.** A record that is active and of type 3 skips the AI
object and the acquisition record (`0x46489D..0x4648A5`), so a puppet seat holds neither and the
takeover makes both, in the init's order. **The teardown `0x464A00` frees both**: `0x40B390` with the
record's map bit, then `0x408F10` and `0x4B4F20` on `+0x74`, which it zeroes
(`0x464A41..0x464A6B`).

**Live rewind prerequisites [DISASSEMBLED and MEASURED 2026-10-02].**
[The S4d experiments](../tadr-port/demo-recorder-s4-rewind.md) call the same routines
from the game-thread frame visit before `0x4969CD`. For a newly seated spectator,
`0x464700(rec)` supplies sight `+0x7C`, groups `+0x78` and statistics `+0xEC`,
while its active-type-3 branch skips the AI objects. Repeated seating must reuse
these buffers: its call `0x480190(rec)` at `0x464898` allocates `0x140` bytes
through `0x4D83B0` and overwrites `+0x78` at `0x4801B1` without freeing the old
pointer; the remainder initializes ten `0x20`-byte groups and returns `ret 4`
at `0x4801E3`. P1 reused all three buffers across two activations at GameTime
1522 and then completed teardown and a second skirmish load.

**The mid-game spectator must receive the loaded seat's metadata [DISASSEMBLED].**
The ordinary player init writes GameTime into `+0xF4` (WinLoseTime) at `0x464727`;
a fresh playback initializes it at zero, while a mid-game init would stamp the live
GameTime. Its only recorded consumer is the saver. Rewind initializes the viewer's
stamp to zero so its frame matches a fresh playback.

`0x451090` has direct callers `0x45119D`, `0x451584` and `0x4540BB`. It selects
the controlled record's PlayerInfo, copies `main+1` to `info+0xA7` at `0x4510C0`
and `main+2` to `info+0xA8` at `0x4510D4`, and inserts `main+0x2A3C`'s low nibble
into `info+0x9B` bits 0–3 (`0x4510E4..0x4510F4`). The two version bytes also
have stores at `0x4513F1` and `0x451400`. The loader sets `info+0x9B` bit 4 at
`0x497BFF`, then calls `0x450F90` and `0x451180` at `0x497C06` and `0x497C0B`.
The former returns immediately without the network bit (`0x450F9B..0x450FA2`);
the latter reaches the metadata fill described above. Rewind supplies these
bounded scalar fields for V without rerunning the loader or its network paths.

The scheduler's player scan tests active/type 3 at `0x4952CF..0x4952D6`, then
skips a zero live count `+0x144` at `0x4952D8..0x4952DF`, before reading the
pacing stamp `+0x18` at `0x4952E1`. An empty spectator therefore needs no stamp
write. P1 measured 318 ticks in 10.454 seconds with that seat controlled.

For the reverse conversion, P2 killed 1370 live units while all four seats still
held their local types and AI objects. After zero units remained, the type
setter and the teardown sequence above cleared both objects; rebuilding made
1416 puppets with zero refusals. `0x40B390(seat)` clears `0x5119C0[seat]` at
`0x40B514` and returns `ret 4` at `0x40B51E`; `0x408F10` receives the AI object
in ECX and `0x4B4F20` receives it as a caller-popped argument, as shown at
`0x464A5B..0x464A68`. The experiment covers the battle fixture, not every order
handler, and does not test return-to-now or the live writer handshake.

**A create picks the proxy movement class only for a remote owner**: `[owner+0] != 0 && owner+0x73 ==
3` (`0x43DC4A..0x43DC57`). So the seats' types must be set after the last kill and before the first
create.

**The radar pass's loop 4, `0x467680..0x4676FD`** (unit at `esi − 0x92`): for a unit with `+0x110`
bit 28 whose owner is active and of type 1 or 2 (`0x4676A1..0x4676AA`) and whose def has `+0x245`
bit 13, it calls `0x40B0D0(unit+0xFF, unit+0x6A, def+0x208)` and on a nonzero answer stores
GameTime + 90 to `+0xB0` and sets `+0x110` bit 12 (`0x4676C8..0x4676E8`). **`0x40B0D0` reads
`0x5119C0[seat]` untested** (`0x40B0E5..0x40B0EC`), so the acquisition records must exist before the
first radar pass after the flip. It walks the record's list from `+0x09` to `+0x0D` and leaves at
once when they are equal (`0x40B0EC..0x40B100`); the constructor zeroes both (`0x4091A1`,
`0x4091A4`), so the pass at T sets neither field for a record made at T, unless something between the
construction and the pass appends to the list [not read].

**The order dispatchers clear `+0x06` before each handler call**: `0x43B7C0` stores 0 at `0x43B83D`
and `0x43BAD0` at `0x43BB09`, after reading `+0x0A`, the deadline, against GameTime
(`0x43B7E2..0x43B7FB`, `0x43BAF8..0x43BAFD`). A handler arms its events afresh on every call, which
is why a node rebuilt with `+0x06` = 0 waits for nothing but its next call.

**Handler entry facts the rebuilt nodes rest on:**
- MobileBuild `0x403A20` tests event 8 before its state table (`0x403A27`), and its state 3 has no NULL
  test of the target (`0x403E43`, `0x403EEB`). The target's death fires event 8 through the node's
  vtable slot 0 (`0x489762`) before clearing the reference (`0x48978E`), in `0x489740`'s walk of
  the target's `+0xA2` chain. Its heading before `0x438590` is `0x48A980(builder+0x6A,
  target+0x6A) − builder+0x66` (`0x403DCA..0x403DE1`).
- BuildingBuild `0x402640` tests event 2 at its entry (`0x402647`) and event 8, the frame's death,
  before its states (`0x402726..0x402751`: the reply `0x47F780(unit, 7, …)`, `0x41C150`, return 0).
  Its state 3 tests the nanoframe `+0x16` for NULL (`0x4029CA..0x4029CF`) and returns 7 on one
  (`0x402B4D..0x402B59`), and the dispatcher's case 7 (`0x43BA3D..0x43BA6D`) destroys the unit's
  whole main list through `0x43A1F0`. It arms itself with the timer `0x439E80(node, 1)` and `| 0xA`
  (`0x402ADC..0x402AE6`), as MobileBuild does (`0x403F00..0x403F0C`): `0xB` in all.
- The slot-weapon readers `0x49ABB0` and `0x49ADF0` take a slot as the low byte of their argument
  and read `[unit + slot·0x1C + 0x10]` with no NULL test before dereferencing it (`0x49ABCB` then
  `+0x111` at `0x49ABCF`; `0x49AE04` then `+0xDC` at `0x49AE08`).
- BuildWeapon `0x402B70` reads the slot `p1` (`node+0x36`) at entry, `[unit + p1·0x1C + 0x10]`
  (`0x402B7F..0x402B89`), and in state 2 increments the byte `[unit + p1·0x1C + 0x1E]` (`0x402BB3..0x402BB9`).
- Paralyze `0x402D10` clamps its p1 to `0x708` ticks (`0x402D33..0x402D3B`).
- Attack_Chase `0x4034A0` picks a slot when p1 is 0 and stores it to `+0x36` (`0x4035A1`).
- `0x439E80` is the node's timer, `thiscall (node, ticks)`, `ret 4`: `+0x06 |= 1` and `+0x0A =
  GameTime + ticks` (`0x439E80..0x439E9A`). Four of the arming stores above follow a call of it
  (`0x4020FA`, `0x402121`, `0x4041F8`, `0x4142D5`).
  It has 75 call sites, and GameTime is `[main+0x38A47]` (`0x439E8E`), so a re-armed list's image
  changes every tick: MEASURED 2026-10-10 on format-9 recordings, 286,289 of the battle's 293,393
  `RK_ORDERS` meet the last list of the same (slot, list) at the same length, and every one changed
  its head node (node 2: 59 times, node 1: once); Town & Country 161,642 of 167,290, the air war
  48,619 of 52,199; the longest list held 3 nodes. The recorder's orders frame codes a list
  against that last image (`tagpu_recframe_orders.c`;
  [*The demo recorder's record frames*](#the-demo-recorders-record-frames-the-engine-facts-they-predict-from-disassembled-measured-2026-10-10)).
  The 75 is every `call 0x439E80` in the executable (no `jmp` to it), and the code
  `0x439E80..0x439E9D` was re-read on 2026-10-10: `or` of `+0x06` with 1, `[main+0x38A47]` plus
  the argument into `+0x0A`, `ret 4`.

**The sim step's head, `0x4954A0..0x4954ED`**, is read in
[*the tick boundaries*](sim.md#the-demo-recorders-tick-boundaries-the-sim-step-0x495490-and-its-callers-disassembled-2026-10-01):
the GameTime store at `0x4954C0` comes before the unit tick's call, which is why the recorder arms a
fork between two steps and its first capture is iteration T.

**Negative result: the takeover writes no skirmish settings.** A scan of the callee closure of the
nine functions above, to depth 4 (288 functions, each body cut at the next call target of the
image), finds no operand `0x29A0` (the settings block `[main+0x29A0]`) or `0x38D81` (its entry
count). Impure's own code writes the difficulty `main+0x37EEE`, which the replay's options restore at
the level's end.

### The replay viewer's record: player record 10 and the level init's call `0x4919C8` [DISASSEMBLED 2026-10-06]

Stage 5 of the [multiplayer slice](../tadr-port/demo-recorder-multiplayer-slice.md) seats a playback's
viewer in player record 10 on every playback ([the ten-seat note](../tadr-port/demo-recorder-ten-seats.md)).
Read from `pristine/TotalA.exe.pristine` (`objdump -d -M intel`).

**What we patch.** The level init's only call site, `0x4919C8` (`E8 C3 2F FD FF`, `call 0x464990`,
inside `LoadGameData_Main 0x4917D0`, which the loader thread calls at `0x497581`), is retargeted to
`tagpu_replay_level_players` (`tagpu_replay.c`), byte-matched by `call_is` at attach and installed
with the replay's other call sites, all or none. It calls `0x464990` itself and then, only on a level
whose seating armed it, the player init `0x464700` on record 10. `0x464990` takes no argument and
returns with a plain `ret` (`0x4649C6`), and the next instruction, `call 0x44F6A0`, reads no register
the call leaves (`0x44F6A0` starts with `push 0xC9; call 0x4B4F10`), so the C function is the call's
target directly. A whole-image scan finds no other `call` or `jmp` to `0x464990`.

**Record 10.** The records are `0x14B` bytes from `main+0x1B63`; record 10 is `main+0x2851..+0x299B`
(the teardown's bound, `add eax,0x2851` at `0x464A0C`), followed by the branch selector at
`main+0x299C`.

| Address | What it does with record 10 |
|---|---|
| `0x41D97E..0x41D997` | the constructor loop runs 11 times (`ebx` = 0xB), each through `0x463BE0`, which makes the record's PlayerInfo |
| `0x4644D0` the players reset | zeroes all eleven records but `+0x27`, the PlayerInfo pointer, which it reads first and stores back (`0x4644D7..0x46450F`, `cmp edx,0xCEE` / `jle`), then sets `main+0x2A42` = 0 (`0x46451B`) and `main+0x2A43` = 0 (`0x46452D`) |
| `0x464290(seat, type)` the seat activation | `stdcall`. Writes only record `seat` and its PlayerInfo: the five 11-byte rows `+0x108`, `+0x113`, `+0x11E`, `+0x129`, `+0x134` zeroed (`0x4642BE..0x464329`), `+0x73` = type (`0x464332`) and PlayerInfo `+0x94` = type unless 3 (`0x46433A`), `[+0x113+seat]` = 1 (`0x464342`), `+0x74` = 0 without a free (`0x46434A`), `+0x146`, `+0x148`, `+0x147` = seat (`0x46434D`, `0x464353`, `0x46436B`), `[+0x108+seat]` = 1 (`0x464359`), `[rec+0]` = 1 (`0x464364`), `+0x13F` = 5, PlayerInfo `+0x9B` bit 5 cleared (`0x464378`), `+4` = seat, `+0xC` = 0, `+0x22` = 0 (`0x464381..0x46438C`); a type 1 or 2 also gets PlayerInfo `+0x99` (`0x4643BC`); in game mode 1 or 2 the name `+0x2B` is copied to `+0x49` (`0x464418..0x464439`, `0x464497..0x4644B8`), a type 1 or 2 first having a name formatted into `+0x2B` through `0x4E42B0`; `ret 8` (`0x4644BE`) |
| `0x464990` the level init's players | `cdecl (void)`: `0x464700` for each of records 0–9 whose `+0x73` is set (`edi` = 10 at `0x464994`, the type at `main+0x1BD6 + i·0x14B`), then `0x4648E0` (`0x4649BF`) |
| `0x464700` the player init | `stdcall (rec)`, `ret 4` (`0x4648D6`); its one direct call is `0x4649B1`. Sets `+0xF0`, `+0xF4`, `+0xF8` to GameTime (`0x464715`, `0x464727`, `0x464765`), zeroes the resources and storage (`0x46476B..0x4647A1`), makes `+0xEC` when NULL (`0x4647CB..0x4647D7`: `0x4B4F10(0x34)` at `0x4647CF`) and calls `0x401070(+0x146)` on it (`0x4647EA`), frees the old sight counters `+0x7C` (`0x464855`) and allocates new ones of `(W/2)·(H/2)` rounded up to 8 bytes (`+0x88`, `0x46482B..0x464866`), zeroed, then the group table through `0x480190` (`0x464898`), which allocates `0x140` bytes through `0x4D83B0` (`0x4801A1`) and stores them to `+0x78` (`0x4801B1`) without freeing the old table. It makes the AI object and `0x40B320(+0x146)` unless `[rec+0]` ≠ 0 and `+0x73` = 3 (`0x46489D..0x4648A5`) |
| `0x464A00` the player teardown | walks records 0–10 (`cmp` against `main+0x2851` at `0x464A99`, `jbe` at `0x464A9B`): frees `+0x7C` and zeroes `+0x7C..+0x88` (`0x464A24..0x464A38`), frees `+0x78` through `0x4801F0` (`0x464A3C`), frees the AI object only when `+0x74` is set, with `0x40B390(+0x146)` first (`0x464A41..0x464A6B`), and frees and zeroes `+0xEC` (`0x464A6E..0x464A7E`, the free at `0x464A76`) |
| `0x4816A0(m)` the sight rebuild | with `m` ≠ 0 fills the explored map with 0 or `0xFFFF` by Mapping (`0x4816AF..0x4816FB`); its counter fill loops records 0–9 (`cmp bl,0xa`, `0x481707`, `0x481777`) and skips a record whose `+0x146` is 10 (`0x48172A`), so record 10's counters are never refilled |
| `0x48C190` the ID cycle | walks the controlled record's ID range `[+0x6F, +0x71]` (u16, `0x48C1CA..0x48C1D9`); an ID becomes the unit `array + ID·0x118`, and ID 0 a NULL it reads at `+0x110` (`0x48C1EE..0x48C213`: `test dx,dx` at `0x48C1EF` takes ID 0 to `xor eax,eax`; and `0x48C239..0x48C25E`). With `[1, 1]` it reads slot 1 alone |
| `0x4858BD..0x48592B` the level's slot setup | writes `+0x67`, `+0x6B`, `+0x6F`, `+0x71` for records 0–9 only (`cmp edi,0xa`, `0x48592B`) |

**The walkers of a record's unit block test the empty block** [DISASSEMBLED 2026-10-06; the
classification of all 35 reads of `+0x67` is in [the ten-seat note](../tadr-port/demo-recorder-ten-seats.md#3-what-record-10-needs-and-what-the-engine-gives-it)].
Every walk of the controlled or viewed record's block compares the first unit with the last before
it reads one and leaves on `first > last`: the `ja` at `0x41C345`, `0x467516`, `0x48BE3F`,
`0x48CA80`, `0x48D2A5`, `0x48D454`, `0x48D4AC`, `0x48D505`, `0x48D561`, `0x48D67E` and `0x48DD40`
(there `mov ecx,[eax+0x67]; cmp ecx,esi; ja` at `0x48DD3B..0x48DD40`). So record 10's empty block,
`+0x67` = 1 above `+0x6B` = 0, is walked by none of them. **Loops that stop at record 9:** the
players phase (`0x4655A8`), the sight rebuild's counter fill (`0x481707..0x48177A`), the level
start's resource seed (`0x465F87`, `cmp edi,0xD84`) and the post-game table `0x41DC20`
(`cmp ebp,0xCEE` / `jl` at `0x41DEC5`, which also skips a record whose `+0x146` is 10 at
`0x41DD37`).

**The in-play interface takes the viewed record's side** [DISASSEMBLED 2026-10-06]. The loader
loads MAIN2 once, from the viewed record's side (`0x497B56..0x497BD8`): it reads `main+0x2A43`
(`0x497B56`), PlayerInfo `+0x95` through `+0x27` (`0x497B7D`), formats `%sMAIN2.GUI` (`0x509570`)
with the side's name at `main+0x37F5B + side·0x232` into `main+0x37EA0` through `0x4E42B0`
(`0x497BAA`), loads it into the desktop GUI `main+0x519` with `0x4AA8F0` (`0x497BC6`) and stores
`0x494890` into the screen's `+8`, its OnCommand, and `main` into `+0xC` (`0x497BCB`, `0x497BD8`). The chrome
and both bars read the side every frame (`0x467D98..0x467DB3`).

**The explosion's owner gate tests type 3 alone** [DISASSEMBLED 2026-10-06]: `0x49A01B..0x49A047`
forms the owner record from the projectile's owner byte `+0x66` and skips the damage when the
record is active and `cmp byte [rec+0x73],3` (`0x49A043`) holds. The replay admits a multiplayer
recording's remote seats only as type 3 for this reason (`tagpu_replay.c`, the header checks).

**The viewed record's work in the players phase.** The phase loops records 0–9 (`cmp bl,0xa`,
`0x4655A8..0x4655AF`). Only in the viewed record's iteration does it run the minimap rebuild
`0x466DC0` every tick (`cmp bl,[main+0x2A43]`, `0x465065..0x465072`) and, past the record's 30-tick
deadline `+0xF0` (`0x46507D..0x465092`: `ja` past the whole block, else `+0xF0 += 30`), the radar pass
`0x467440` and the minimap's fog `0x466C20` (`0x465565..0x465572`). `0x466DC0` takes no argument and
returns with a plain `ret` (`0x46743D`); its last store is `main+0x142F1 |= 2` (`0x46742F`). Its two
callers are that phase and the sight rebuild (`0x48191F`, after the fog at `0x48191A`). With record 10
viewed none of the three runs, so `tagpu_replay.c`'s `viewer_frame` runs them once a GameTime at the
frame visit. On a paused frame the phase does not run at all, so a replay that rewrites the world or
the viewed record outside a tick — the seek (its step R14d, after the contacts R14b makes and the
expiry; a takeover's after its deselect) and a Sight change — calls `0x466DC0` itself for whichever
record is viewed. MEASURED 2026-10-07 before
that call: in seat 0's view, a paused seek to GameTime 1500 left the minimap with no unit dot until
play resumed. **`0x466DC0`'s unit dots**: it walks the unit array (`0x466E3A..0x466E56`, a slot with
`+0xA6` 0 skipped) and draws a dot for a unit when a draw-every-dot flag is set — LosType
`main+0x14281` bits 0–1 both clear (`0x466E14..0x466E1F`) or the watcher flag `main+0x37F2F` bit 9
(`0x466E23..0x466E32`) — or its `+0x110` bit 8 or 9 is set (`test ah,3` at `0x466E6A`), or its owner
`+0xFF` is the viewed record `main+0x2A43` (`0x466E6F..0x466E7D`); the dot has the owner's colour
whichever gave it (`0x466EC3..0x466F06`). It runs before the step's pass in the players phase, so a
pass's contact bits reach the dots from the next step on. DISASSEMBLED 2026-10-23.

**The load's order** around the patched call, in the loader function: the saved-game branch's seat
fill `0x47A760` (`0x49757C`, behind `0x435100` = 2 at `0x49753D`, which a Start does not reach),
then `LoadGameData_Main` (`0x497581`), whose level init gives each record its buffers, then the sight
rebuild `0x4816A0(1)` (`0x497AE5`), the first reader of the viewed record's counters, through its fog
`0x466C20`.

**The network reporter.** `main+0x2A42` reaches `reporter.dll`'s export only while the library is
loaded: `0x51E58C` is set nonzero only at `0x46C00F` and zeroed at `0x46C11F`, `0x46C149` and
`0x46C1CF` (after `FreeLibrary`). The export receives `+0x2A42` as a value: `0x46C7CC` loads
the byte and `0x46C7E7` calls `[0x51E584]` with it among ten arguments. The replay refuses to seat
while the handle is set.

**The destructor's death cue `0x4869EB`** [DISASSEMBLED 2026-10-06]: `0x494FF0(5)`, stdcall
`ret 4` (`0x495000`), adds 5 to the music's combat ring when the dying unit's killer byte `+0xF4`
equals the controlled index (`0x4869D7..0x4869E3`). `+0xF4` is written by the hit `0x489DC0` as
the attacker's owner `+0xFF`; MEASURED 2026-10-06, it holds 10 on 906 of 1448 deaths in the battle
fixture and on every death in the ten-seat fixture, so with record 10 controlled the cue would fire
on most deaths. The replay retargets the call to a stub that returns, popping the argument, on a
puppet level and otherwise jumps to `0x494FF0` (`tagpu_replay.c`, THE DEATH CUE). The hit cue at
`0x489DE6` compares the attacker's and the victim's `+0xFF` with the controlled index and needs no
gate.

**The resource bar's colour badge reads the viewed record's colour with no bound** [DISASSEMBLED
2026-10-06]: the bar reads the viewed index `main+0x2A43` (`0x468E4B`), forms the record at
`0x468E73` and calls the badge `0x467C00` at `0x4690AB`. The badge reads PlayerInfo `+0x96`
(`0x467C18`), takes the frame `[[main+0x148DB] + 8·colour + 0x28]` (`0x467C24`), uses the frame's
width and height as the quad's uv, and draws it through `GAF_DrawTransformed 0x4C7580` (`0x467CA1`)
at the quad (132,5)-(152,25). The colour sequence `main+0x148DB` has one writer, `0x431911`, at
start-up, and holds ten frames; the players reset `0x4644D0` sets every record's colour to its own
index (`0x46461E`, the loop over records 0..10, `0x4646CB..0x4646D3`), so record 10 holds colour
10. MEASURED 2026-10-06, two processes: the dword past the ten frames (`[seq+0x78]`) is
`0x0001000A`, the first dword of the next heap object, and a full-map playback with record 10
viewed and its colour left at 10 crashed at `0x4C74C7` reading pixel data at NULL. The replay
therefore gives record 10 a recorded seat's colour (`tagpu_replay.c`, `viewer_seat`).

### The demo recorder's perspective: the clicks and the world point [DISASSEMBLED 2026-10-06]

Stage 4c of the [multiplayer slice](../tadr-port/demo-recorder-multiplayer-slice.md#stage-4-perspectives-and-communication)
records each human seat's camera, cursor, clicks and selection
([the perspective plan](../tadr-port/demo-recorder-perspective-streams.md)). Read from
`pristine/TotalA.exe.pristine` (`objdump -d -M intel`).

**The two click redirects.** Each click action has exactly one call site in the image: a scan of
every `E8`/`E9` rel32 in the executable section reaching either entry, and of every absolute dword
in the file naming one, finds the left action `0x498F70` only at `0x4995B3` (`e8 b8 f9 ff ff`) and
the right button's press `0x499100` only at `0x4993AC` (`e8 4f fd ff ff`). The recorder rewrites
both `call`s to 17-byte stubs, `push button; push [esp+8]; call rec_click; ret 4`, and
`rec_click` records the click and then calls the action with the same argument (`tagpu_recorder.c`,
THE CLICKS). Both sites lie in the mode-6 handler `0x499200`, after `0x498DA0` (`0x499221`) has
stored this message's world point and region and after the cursor choice has stored the cursor
index (`0x4992AD`). Each action is `stdcall(record)`, `ret 4`, and starts by saving `ecx` and
reloading main (`0x498F70`, `0x499100`), so it reads no register of its caller's. The record is
the handler's own stack copy of the six-dword mouse record `main+0x2C76` (`rep movs` of 6 dwords,
`0x499210..0x49921A`), its wParam at `+8`, which the left action tests at `0x498FA9` and the right
at `0x49917A`. Impure's own `order_check` calls `0x499100` directly as a cancel and does not pass
the redirect.

**What a click carries** is read at that moment: the armed order `main+0x2CC3`, the cursor index
`main+0x2CBE`, the region bits `main+0x2CC6` & 3, the Interface Type `main+0x37EFA` (a click is
recorded only for 0 or 1), and the world point's integer halves, the `i16` words at
`main+0x2CAC`, `+0x2CB0` and `+0x2CB4` of the 16.16 point `main+0x2CAA`, which `DrawGameScreen`
reads as words too (`0x468DC4..0x468DD2`: `movsx` of `+0x2CB0`, `+0x2CB4`, `+0x2CAC`).

**The reproduction source test** (the features section above) is the stage's other redirect,
made for a multiplayer playing: `0x424185..0x42418E` goes to a stub that asks `feat_source`.

**The RESTART name test `0x460880`** [DISASSEMBLED 2026-10-06]. The exit menu's OnCommand
`0x460800` tests the fired gadget's name with `0x49FD60(gi, "RESTART")` (`push 0x506CA8` at
`0x46087A`, the call at `0x460880`); on a match it pops the screen (`0x4A9660`, `0x46088A`) and
calls `0x4604A0` (`0x46088F`), which loads `RESTART.GUI` (`0x506CD8`) and stores
its OnCommand `0x460340` (`0x4604CB`), which stores 1 to the restart flag `main+0x39249`
(`0x460449`; the flag's other two stores, `0x49129D` and `0x491863`, store a register, and its one read
is `0x4996FB`); otherwise `0x460898..0x46089F` passes the
message on through `0x4AB0A0`. The replay's RESTART gate answers 0 for this one test on a puppet
level, so the menu stays open (`tagpu_replay.c`, THE RESTART GATE).

**The main menu's backdrop call, `0x426447`** [DISASSEMBLED 2026-10-27]. Inside the builder
`0x4263B0`, `call 0x4288D0` at `0x426447` loads the backdrop; the next instruction, `0x42644C`
(`push 0`, `push 0x5031D4`, seven bytes), is the Replays screen's site S2 (`tagpu_replays.c`), after
the builder has written the screen's OnCommand slot `+0x08` at `0x426432`.

**The main menu's return `0x4270A9` and `0x512C80`** [DISASSEMBLED 2026-10-06]. After the main
menu's builder `0x4263B0` returns at `0x4270A9`, the shell reads the dword `0x512C80`
(`0x4270AE`): nonzero takes `0x4270F0`, which sets the shell's sub-state `main+0x2BBF` = 6 instead
of going on in the main menu [INFERRED: the lobby's launch path]. Its other references are a read
at `0x444007`, which takes it as an index 0..4 through the jump table `0x444184` to one of the
four GUIDs at `0x4FCD98..0x4FCDC8` and then zeroes it (`0x44403E`), a store at `0x45B878`, and the
address taken at `0x45B6A1` and `0x45B7D0`. The Replays browser reopens after a Start it made only
while it reads 0 (`tagpu_replay.c`, `menu_built`).

### Objective arming across live rewind

DISASSEMBLED: the argument form of `+kill` calls `0x486F10` at `0x4164DF`,
then calls objective disarm `0x4904B0` at `0x4164EF` even when the selected seat
has no units. `0x4904B0` stores zero at objective `+0x88` and returns. Objective
checks test that DWORD at `0x490239` and `0x490365`. Return-to-now preserves its
pre-conversion value; a fork deliberately arms it under the takeover policy.
The objective pointer is at main `+0x391ED`, and level setup/teardown owns its
lifetime as documented above.

The controlled human seat can remain local with no units. The last-unit
destructor decrements player `+0x144` at `0x486DFC`, then calls the skirmish
notification `0x47BD70` at `0x486E54`. DISASSEMBLED: `0x47BD70..0x47BDE2`
formats a player message and sends it through `0x463CA0` at `0x47BDD5`; it
does not store a new controller type. With objectives disabled, native
`+kill 0` leaves the recorded human seat empty in a continuing level.
Return-to-now must restore that saved state. The nonempty-seat condition in
`tk_bound` applies only to takeover; saved-present restoration retains the
same memory, seat-identity, allocation and objective bounds without that
eligibility test.

### Saved radar contacts on return-to-now

DISASSEMBLED: the players phase compares the seated record's `+0xF0` with
GameTime at `0x46507D..0x465089`, advances it by 30 at `0x46508F..0x465092`,
and calls `0x467440` only for the viewed record at `0x465565..0x46556D`.
The radar pass writes unit `+0x110` bits 8–10 and 12 and may refresh `+0xB0`
(the five loops are mapped above). These are scan history between deadlines;
a scan over the same positions at a later tick need not reproduce them.

MEASURED 2026-10-03 on the reference Wine setup: at paused GameTime 329, a
new human solar plant held state `0x30280221`. Return-to-now's extra radar
pass changed it to `0x30280321`, with no intervening simulation step. The
full battle fixture exposed the same bit-8 error on an enemy nanoframe at
GameTime 1679. The corrected return restores mask `0x1700` from the saved
image for each bounded, identity-matched live slot and omits that extra scan;
`+0xB0` is already restored by the ordinary unit image. A new run at GameTime
318 retains `0x30280221` and its complete before/after frame compares EQUAL.
The seated records' saved `+0xF0` deadlines still schedule the next engine scan.
Historical seeks retain their existing scan and contact policy.

## The replay's players and changing the viewed player

Read while implementing the player cards (deleted since, for the replay HUD's rows) and
`tagpu_replay.c`'s player model and Sight. No new patched address: the replay's frame observer
at `0x4969CD` applies the queued view change. The model and Sight:
[the replay's players and Sight](../tadr-port/demo-recorder-player-cards.md); the HUD that shows
them: [The replay HUD in game](../hud-in-game.md).

| address or field | fact and evidence |
|---|---|
| `main+0x531` | Current top `GUIMEMSTRUCT*`, the same top the menu slot uses; the replay HUD's modal test walks the stack down from it by name (*The in-game quit, YESORNO, a button's frame*). Existing GUI stack disassembly, `tagpu_menu.c`'s `OFF_TOPGUI`. |
| `0x4C1400`, `0x4C1410` | No-argument getters of the text foreground/background, globals `+0x208/+0x20C`; plain `ret`. `0x4C13A0(fg,bg)`, stdcall `ret8`, changes each field unless its argument is -1. The cards saved both, set their colours, drew, then restored; nothing of ours calls them now. DISASSEMBLED `0x4C13A0..0x4C141B`. |
| `0x4C1440`, `0x4C1450`, `0x4C1480` | The FNT face in force, its height, and a string's measured width. `0x4C14F0(ctx,text,x,y,maxWidth)` draws, stdcall `ret0x14`; `ctx=NULL` locks the back buffer. The cards' labels were measured and bounded before drawing; nothing of ours calls them now. DISASSEMBLED getters/measure at `0x4C1440..0x4C14EF`; draw and font-table limits in *The in-game bitmap font* above. |
| `main+0x148DB` | Player-colour GAF sequence, loaded at `0x431911`; u16 count at sequence+0, frame pointers at +0x28 with stride8. `0x467C1E..0x467C24` reads a frame using PlayerInfo+0x96 without a bound. `tagpu_replay.c`'s `logo_colour` bounds the logo by the sequence count, decodes only a nonempty frame at most 64×64 into a fixed plane and keeps its centre pixel's palette index: the seat colour of the replay HUD's rows and markers and of the overlays, taken once a playing (`seat_colours`). The sequence lives with the game's loaded resources. DISASSEMBLED caller and existing replay seat validation. |
| player `+0x8C/+0x90/+0x94`, `+0x98/+0x9C/+0xA0` | Energy and metal current/production/expense floats. Card values come from the reader's recorded player image, not a new economy calculation. Existing `0x401360`/`0x466050` evidence in *The unit and player records*; no exclusion of these fields in `replay-layout.tsv`. |
| player `+0x144`, `+0x106` | Units alive and commander losses are words, written at `0x486DFC` and `0x486996`. These recorded fields feed the replay HUD's rows (the units, the skull, the dimmed row), its marker walker and its stats walker (`tagpu_hudmarks.c`; `+0x144` only, *The replay HUD's stats window and the pause icon*). A loss count alone does not establish whether a commander currently exists. |
| `0x4864CB..0x48650F`, `0x48691B..0x486958` | Commander tests compare the unit definition's UnitName at +0x20 with the owner's side's commander name, case-insensitively. The latter indexes `main+0x37F5F` by side×0x232. `tagpu_replay_players` applies the test to recorded live units, bounding owner and definition IDs and using the recorded seat's side, into `commander_alive`, which nothing reads since the cards went. DISASSEMBLED; also `tagpu_recorder.c:is_commander`. |
| `0x431A20`, `0x431A60`, `0x431B6D` | Side-table free loop advances0x232 until0xAFA (`0x431A4A..0x431A56`): five records. The loader starts at `main+0x37F3D` and requests a32-byte commander name (`0x431B6D`), matching the bounded compare. `tagpu_replay_players` requires side below both five and the loaded count at `main+0x37F39`. DISASSEMBLED. |
| `main+0x2A43`, `0x482130`, `0x482910`, `0x466C20`, `0x467440` | Viewed seat, temporary sight expiry/add, minimap fog and radar pass. Sight drops the old view's temporary records before writing the new viewed byte, recreates unexpired death vision from a separate recording reader, then runs the fog/contact passes. The existing seek's sight notes establish the pool bound, ownership of stamps and engine root contracts; the request adds no direct counter writes. The current frame's recorded display state is applied before the view request. The replay HUD's Sight asks it (`tagpu_replay_sight_request`), and -1, its pressed Sight clicked again, the full map. |

Measured on the reference setup: a paused switch through two seats and back to the spare viewer
kept GameTime 476; a switch to the full map kept a separately checked eye at (1800, 1900). These
are input and camera-isolation checks; the death-vision comparison is the `battle-los` one in
[the replay's players and Sight](../tadr-port/demo-recorder-player-cards.md), *Verification*. Through
the replay HUD (2026-10-09), Sight on seat 2 and the pressed Sight clicked again (record 10).

## The replay HUD's stats window and the pause icon

Read for the replay HUD's stats window ([The replay HUD in game](../hud-in-game.md), *The stats
window*). One patched address, the pause icon's test; everything else is read. The stats walker
reads **the recording's image** of each record through its own `RecReader`, never engine memory;
only the unit types' values come from the engine, copied on the game thread.

| address or field | fact and evidence |
|---|---|
| `0x46A107` (DrawGameScreen `0x468CF0`) | **Patched.** The pause icon's test, `test byte [eax+0x38A51],1` (`F6 80 51 8A 03 00 01`) then `je 0x46A140` (`74 30`), `eax` = main from `mov eax,[0x511DE8]` at `0x46A102` (`A1 E8 1D 51 00`). `0x46A110` draws `igpaused` and goes on at `0x46A14E`; `0x46A140` loads the same `esi`/`edi` and falls into `0x46A14E` (*The frame's HUD extras*). `pause_icon_init` (`tagpu_replay.c`, THE PAUSE ICON, from `tagpu_replay_init`) checks the nine stolen bytes and the five before them, then lands a stub: the test repeated; when the bit is set, `tagpu_hudinput_on_screen` called (cdecl, inside `pushad`/`popad`, which leaves ZF as the `test eax,eax` set it), and `0x46A110` taken only when it answers 0, else `0x46A140`. It answers 1 when a control on the screen carries the pause: controls in the command record the in-play draw took before this point, and they are the replay HUD's (a recording playing), or the live HUD's while a rewind owns the level (`tagpu_hudstate_mode`, *Live rewind* in [the HUD in game](../hud-in-game.html)). Game thread, inside the draw; it writes nothing. A replay with the replay HUD on the screen draws no `PAUSED`: the HUD's play button carries the pause, as the mockup shows (the owner's decision, 2026-10-09); nor does a live rewind's strip, which holds the game. A live recorded skirmish the player pauses keeps the icon: the live HUD idle is one button with no pause on it. GDI, a refused HUD and every level that is neither keep the icon. DISASSEMBLED; MEASURED 2026-10-09: the log's `replay: the pause icon's test 0x46A107 in`, and a paused replay with the stub shows no icon. |
| `main+0x1439B`, `main+0x1438F` | The level's unit defs (stride `0x249`) and their count, UNITINFOCount. `tagpu_replay_army_values` reads each def's value on the game thread, before the walker's `CreateThread`, into a table the walker owns until `walk_reap` frees it after the join. The defs are the load's and only the teardown frees them. MEASURED: `279 unit types valued` on Two Continents. |
| def `+0x18A` | The metal cost, a float (`0x438650` takes `max(+0x18A, 10.0)`; `0x402640` credits it × (1.0 − the target's build left), row `0x402640` above). A type's army value. |
| def `+0x22F` | 1: the type moves — the test `0x4862CF` gives a unit its mover by (*The unit and player records*, `+0x00`). A type that does not move is worth 0. |
| def `+0x245` bit `0x40000` | The FBI `commander` tag (DISASSEMBLED at `0x42CBAB`, above). A commander is worth 0 (the owner's choice, 2026-10-09). |
| unit `+0xA6`, `+0xFF`, `+0x104`, `+0x110` bit 28 | From the recording's unit images: the type index (u16), the owner (byte), the build left (a float; finished when it is 0.0 by the engine's own test, the C3 of an x87 compare as at `0x4586DF`, `build_finished` in `tagpu_replay.c`) and alive (`0x10000000`). A unit counts toward its owner's army value when its slot is live, bit 28 is set, it is finished, its owner is below 10 and its type below the table's count; a type index past the count is data the recording holds and is worth nothing. |
| player `+0x90`, `+0x9C`, `+0xD4`, `+0xFC`, `+0xFE`, `+0x144` | From the recording's player images, for the recorded seats: energy and metal income (floats), MetalWasted (a double, written by the economy pass's clamp `0x401AB3..0x401B27`), kills and losses (u16), units alive (u16). |

`tagpu/ddraw/replay-layout.tsv` excludes none of these unit and player bytes: no unit row covers
`+0xA6`, `+0xFF` or `+0x104` (the nearest are `+0xA2..+0xA5` and the IDs at `+0xEC` and `+0xF0`),
and its `+0x110` rows mask `0x10`, `0xC0` and `0x3C00000`, not bit 28; no player row covers the
player bytes. The sampling, the
period and the publication are in the hud-in-game note.
