# Demo recorder, playback and the replay HUD

Recording every game, watching a recording, and the replay HUD.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.6e The demo recorder (`tagpu_recorder.c`, `tagpu_recwriter.c`, every game by default)

S1, S2a, S3a, S3b, S3c-2 and S3c-3 of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s1-the-recorder): every
skirmish and native multiplayer game is recorded to `replays\<date> <time> <map>.imprec`, and
`tools/replay/tarec.py` reads a recording back. `tagpu_recorder.on` is a play default
(`tagpu_opt.c`'s table), read with `tagpu_opt_on` at the level's first iteration, so a game is
recorded unless `tagpu_recorder.off` or `tagpu_defaults.off` is in the game's folder; a tacli
instance's bare launch writes the second, and `tacli arm <i> recorder.on` records there. The lever
decides whether a game is recorded, not what the simulation runs. Its capture has no site of
its own: B2's stub on the unit tick's call (`0x4954ED`, §2.6c) calls the replay's
`tagpu_replay_capture` (inert unless the level plays a recording, §2.6f) and then
`tagpu_rec_capture`, both before the air pool's rebuild, on the game thread, and `tagpu_reclaim`'s
teardown wrap (§2.7) calls `tagpu_rec_level_end` first, before the cascade frees the unit array.
A recording does not start without that wrap, since nothing else would end it. **One Win32 hook**:
`ExitProcess` in the fork's kernel32 list (`hook.c`, the exe and the DLLs of its folder only), whose
`fake_ExitProcess` (`winapi_hooks.c`) calls `tagpu_rec_exit` first, as the fork's own exits do,
which the hook does not reach (`SC_CLOSE` in `wndproc.c`, `tagpu_refuse`, and the out-of-memory
and unit-type refusals in `tagpu_patches.c`): the process's exit waits until the file of every
recording that has ended is closed ([*Its lifetime*](../tadr-port/demo-recorder-skirmish-slice.md#its-lifetime)).

**The prune** (`tagpu_recfile.c`; THE PRUNE in `inc/tagpu_recfile.h`): each new recording, never a
fork's, asks a recording-file worker to prune `replays\` as its writer starts (`recfile_request`,
`REC_FILE_PRUNE`); a full queue or an exiting process refuses the request, and the log says
`recorder: no prune at <file>: no file worker`. The other `replays\*.imprec` are taken newest first
by the start each one's header records (`RT_WALLCLOCK`'s FILETIME in UTC, read by
`recr_wallclock_of`: one open and one read of the header block a file, on the worker), never by
the file's dates, so a replay copied in counts by when it was recorded. A file with no recorded
start sorts after every dated one, then by name, latest first: one that is not a recording; one of
ours whose header lacks the entry at the 12 bytes this format writes, which every recording of
format 24 or earlier is (it wrote the start alone, in 8 bytes); and one that cannot be opened or
read at the moment (`recr_wallclock_of`'s −1), which is never handed to the deletion and is
counted in use whatever its place, since it might read again, as new as any, by the time the
deletion reached it. **"Newest" is the recording's own date**, the owner's rule: the latest start
by the clock of the machine that recorded it. So watching a replay does not keep it, and neither
does exporting one (an export carries its root's header and start); a favourite or a bookmark
does. A clock set back makes each new recording the oldest at the next game's prune, and a file
dated in the future holds one of the kept places until the clock passes it. The Replays browser
lists in this same order (`recfile_newer_first`, which `scan` in `tagpu_replays.c` sorts by too),
each file's start kept in its `index.dat` line with its root key and read again only with a new
key, so its `ENT_MAX` cut lists the recordings the prune keeps first; a file whose key or start
cannot be read at the open sorts as the prune sorts one it cannot read, after every dated one. The first `REC_KEEP` −
1 = 49 are kept, so the newest 50 are kept counting the new one. The lever's text `keep N` changes the count (`keep_count`, `tagpu_recorder.c`): with no
number, or `keep 0`, it is `REC_KEEP`; `keep 1` keeps the new one alone. Each older recording is
deleted unless it is in use, has a live fork, is a favourite or bookmarked in `replays\index.dat`, or
is not one of our recordings. One of ours in another format (`recr_format_of`) is deleted like any
other unmarked one: this build can neither read its forks nor play it, and keeping them would leave
every format change's recordings on disk for good. **Nothing in use or marked can be deleted, by an ordering** (the
comment above `prune_one`): the prune takes the recording's lock exclusively, as every writer does
(`recfile_lock`), so no append, fork, deletion or compaction runs on it from then on; reads its
timelines, and a live fork keeps it; opens the recording itself exclusively with DELETE access,
which fails while anything holds it open and, while held, lets no playing start, a playing being
the only way a bookmark is added; and then, under the index's lock, which `index_save` also takes
(`idx_lock`, `tagpu_replays.c`), reads the file's marks off the disk — its line, found as the
browser finds it (the name exactly, else the name in another case; a second line of the name
merged in), counts when it is of the root key read through that same handle (`recr_root_key`: no
open by path can share it), or has its key unknown (a line the version-1 conversion or a merge
left) — and marks it for deletion on that handle (`tagpu_replays_unmarked_delete`); an index of
any version but 3 (version 1 until its conversion, an earlier build's version 2), one that cannot
be read, or one that is not text decides nothing, so every recording is kept. It goes when the handle closes, and a mark the
index held before that read keeps it. The lock file goes with the recording, through the exclusive handle
no other caller can share, and so does a lock file the prune created itself for a recording it
keeps; every other caller's lock goes at its release when its recording does not exist
(`recfile_unlock`, [the forks note's *File ordering and lifetimes*](../tadr-port/demo-recorder-s4-forks.md#file-ordering-and-lifetimes)),
so a later touch or export of a pruned recording leaves no lock behind. `SetFileInformationByHandle` is resolved at run time, so the DLL still loads on XP; where it
is absent the prune deletes nothing and counts every older recording as in use. One line per
prune: `recorder: prune at <new>: N older recordings, the newest K kept; of the rest, D deleted, M
kept (forked or marked), B in use, U unreadable`, where *in use* also counts a recording whose lock
or file could not be opened exclusively, one whose root key could not be read, one whose header
the date pass could not read, and every recording while the index could not be read, is of
version 1 (not yet converted) or of a version this build does not know. The process's exit waits
for the worker (`recfile_exit`), so the prune stops at the next file once the exit has begun, in the
date pass as in the deletion, and a date pass the exit cut short deletes nothing.
**What the ordering does not cover:** a favourite given while a prune runs, after it has read that
file's marks, or one the index could not save (`index_save` failing to write `index.tmp`, or the
index over `INDEX_CAP`), does not keep its file. The tareplay forks test runs the prune on a seeded
folder and checks its line and what is left (`prune_step`), and that an export asked of the
pruned plain copy is refused and leaves no lock of a deleted recording; the seeded files' last-write times run
against their recorded starts, so a prune that ordered by last write would fail it. MEASURED
2026-10-22, on the build that ordered by last write, on an instance with play defaults on and no lever file: 55 older recordings, the newest 49 kept
beside the new one; of the other six, the 2 oldest plain copies deleted, a favourite, a bookmarked
file and a file with a live fork (made by a real takeover) kept as forked or marked, and a file
that is not a recording kept. With `keep 3`, the next game's 54 older recordings: the newest 2 kept, 48
deleted and the same four kept. **Not exercised:** *in use*, since a file held open by another process
could not be reproduced under Wine on the reference setup. MEASURED 2026-10-23 by `prune_step` in
the landing gate, both builds, the seeds' last-write times against their recorded starts and one seed in format 24's shape: `6 older
recordings, the newest 1 kept; of the rest, 2 deleted, 2 kept (forked or marked), 0 in use, 1
unreadable`, the format-24 copy among the deleted and the first plain copy kept. **The order's
cost**, MEASURED 2026-10-23 under Wine on the reference setup by a scratch harness calling
`recr_wallclock_of` over 50 copies of a gate recording (a header block of 11,132 bytes each) with
the page cache warm: 6.4 to 7.4 ms for the 50 over ten passes in two runs, about 0.14 ms a file; a
cold cache was not measured. The same harness on single files: 1 for a gate recording and for a
copy with `RT_FORMAT` 23 and the 12-byte entry, 0 for a copy in format 24's shape (an 8-byte
entry), a file that is not a recording and a 3-byte file, −1 for a missing file and for one the
harness held open exclusively. A name too long
for its lock path is counted unreadable before the sort, outside N.

**Its events have 68 sites of their own** (the attach log's `recorder: 68 event sites in`,
MEASURED 2026-10-23), byte-matched and built at `DllMain` by `tagpu_rec_init`, all before any
lands; no recording starts without them, so a patch or mod holding other bytes at any of them --
Teleport's set-position call `0x406BC0` among them -- leaves the recorder off, the log naming the
site. Thirty-one observers run a handler and then the stolen bytes: the COB start entries
`0x4B0B00` and `0x4B0940` (the slice markers' start set, below), the constructor
reservation `0x486075` (the multiplayer begin root, below), the create's success exit `0x4861B3`,
the destructor `0x4866D0`, the corpse helper `0x486360` (`FR_CORPSE`), the build done
`0x41B8D0`, the state setter `0x48B090`, YARD_OPEN's write `0x47DAF1` (mid-function, its
yard recorded as `RK_EV_YARD`), the broadcast starts `0x456190`, `0x456290` and
`0x456218` (inside `0x456200`), the replies `0x47F780` and `0x47F850` (the setter and the replies
chained onto the puppet gates there, §2.6f, so a refused call is neither recorded nor counted), `QueryNanoPiece` `0x43E400` (its query recorded as `RK_EV_QUERY`, below),
the nano emitters `0x4720D0` and `0x472200`, and the order dispatch at `0x43A216`, `0x43B878` and
`0x43BB1D`; and, for the features (S3a), the feature tick `0x424050`, the spawn `0x423C50`, the
destroy `0x4246B0`, FeatureDie `0x423550`, the burn `0x4233A0`, the damage `0x4244B0` and the swap
`0x423710`, each with an `after` at its return and chained onto its puppet gate (§2.6f), the
footprint writers `0x47CC30`, `0x47D0E0` and `0x47C790`, whose `after` runs only on a puppet level
(§2.6f) and which also check each call's place in the step's order, and the attach `0x48AB70`,
its acting attaches recorded as `RK_EV_LINK` (`tagpu_recfmt.h`, *THE CONTEST ORDER*). Ten more `call`s go through stubs that
call the engine's own function: the feature code's six calls of the simulation RNG `0x4B6C30`
(`0x4234C2`, `0x423A38`, `0x423B40`, `0x424113`, `0x424159`, `0x424167`), the destructor's
explosion `0x49B000` at `0x486D50` ([the engine map](../exe-reverse-engineering.md#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30)),
Teleport's set-position `0x48A9F0` at `0x406BC0`, its move recorded as `RK_EV_PLACE` before the
call, and the two click actions' only call sites, the left action `0x498F70` at `0x4995B3` and the right
button's press `0x499100` at `0x4993AC`, each a 17-byte stub that records the click
(`RK_CLICK` in a skirmish recording, `RECMP_CLICK` from a multiplayer game's contributing game
thread) and then calls the action with the same argument
([the engine map](../exe-reverse-engineering.md#the-demo-recorders-perspective-the-clicks-and-the-world-point-disassembled-2026-10-06)).
Reproduction's **source test** `0x424185..0x42418E` (`mov ecx,[esp+0x10]; cmp word [ecx],0; jne
0x4241A8`) is redirected whole to a stub that makes the test through `feat_source`: the stock
test on the engine's own cell pointer, recorded as `FS_SOURCE` by a multiplayer recording, and
answered from the recording on a multiplayer playing's host, whose slots follow the owners'
units; it jumps on to `0x42418F` or `0x4241A8` with `eax` and `ecx` as stock left them. Fifteen `call`s of the COB starts `0x4B0A70` and `0x4B0940` (HitByWeapon, TakeDamage,
TransportPickup, TransportDrop, BeginTransport, setSFXoccupy, TargetCleared, EndTransport) go
through a stub that records and then jumps to the function, and so do six calls of the query
`0x4B0BC0` (BuildingBuild's `QueryBuildInfo` `0x4027FB`, VTOL_Pickup's `QueryTransport`
`0x4113B1`, VTOL_Landing's four `QueryLandingPad` `0x411A35`, `0x411AA6`, `0x411BF5`,
`0x411CF1`): with `QueryNanoPiece`, the COB queries only an order handler makes, each recorded at
its place in the step as `RK_EV_QUERY` and its COB noted in the slice markers' start set
([the engine map](../exe-reverse-engineering.md#the-owners-cob-queries-disassembled-2026-10-12)).
The broadcast observer `0x456218` leaves out the broadcast from `0x411403`, which follows
BeginTransport's recorded start. The five fire functions' network tests (`0x49D7A0`, `0x49DA92`,
`0x49DC64`, `0x49DE25`, `0x49DFAF`) are released while a skirmish is recorded, so that each builds
its `0x0D` on its own stack; the extra weapons' `wpn_tx_fired` hands it over at the send and skips
the send when there is no network (§2.6b); four of the released blocks also run the extra weapons' slot-index splice, which writes no simulation state (`cb_slot_index` says why). That release is the only change to what the engine
does. The shots and the interceptions come through the extra weapons' callouts, and the extra
weapons' own TargetCleared through `tagpu_rec_start_named`. Addresses, registers and what each
hook reads: [the engine map](../exe-reverse-engineering.md#the-demo-recorders-event-sites-disassembled-measured-2026-09-30).

**The counts (S3c-2): 21 count-only sinks**, `tagpu_detour_observe` with no `after`, byte-matched
each on its own at `tagpu_rec_init` (`cnt_sinks_init`; one whose bytes differ is left out, named in
the log, and its bit in the dump stays clear, which `tarec` reads as a site it cannot compare; the
log reads `recorder: 21 of 21 sinks counted`): the explosion add `0x420A30`, the COB thread
allocation `0x4B08C0`, the sound entries `0x47F0C0`, `0x47F1A0`, `0x47F210` and `0x47F300`, the
projectile init `0x49C740`, the shot paths `0x49CDE0`, `0x49CC20` and `0x49C9C0`, the impact
`0x499EB0`, and the particle emitters `0x471FD0`, `0x472330`, `0x472430`, `0x472530`, `0x472630`,
`0x472810`, `0x4728F0`, `0x4729D0`, `0x472AB0` and `0x472C50`. Each reads its return address and,
for `0x4B08C0`, the script's name through the engine's own bound; none writes anything. The
existing observers count too (the roots: the create's exit, the destructor, the build done, the
state setter, the replies, the nano emitters and the seven feature functions). Prologues and why
each is a sink:
[the engine map](../exe-reverse-engineering.md#the-demo-recorders-counts-and-the-steps-phases-sinks-impacts-emitters-and-the-images-gates-map-disassembled-measured-2026-10-01).

**The step's phases**, two call-site redirects in the fail-closed table (`fix_step_phases`,
`tagpu_patches.c`; the log line `enginefix: the step's phases for the demo recorder and its replay
(0x495513 0x495539)`): the step's calls of the projectile tick `0x49B720` at `0x495513` and of the
explosion tick `0x420F30` at `0x495539` call stubs that call `tagpu_rec_phase` (1, then 2), the
engine's function, then `tagpu_replay_phase`, keeping every register and the flags. The recorder
stamps each feature root with the phase it was made in; `tagpu_rec_capture` sets the phase back to 0.
Off a puppet level `tagpu_replay_phase` returns at once.

**The script slice**, a call-site redirect in the fail-closed table of both builds
(`fix_script_slice`, `tagpu_patches.c`; the same log line, `the script slice 0x48ADDF`): the unit
tick's 17-byte script call `0x48ADDF..0x48ADF0` jumps to `pushad; push esi; call unit_slice; add
esp,4; popad; jmp 0x48ADF0`, which gives the unit's slice from C (`tagpu_slice_give`,
`DoScriptsNow 0x4B0D60` with `dt` 1, counted at `0x48ADEB` by where it was given). On a recording
level it first writes `RK_EV_SLICE` when the unit's COB object had an engine start since the
capture point -- the start set the two observers on `0x4B0B00` and `0x4B0940` fill, keyed by the
COB's address and emptied by an epoch at every capture point -- and on a level that plays a
recording the replay may defer the slice to its marker (§2.6f). Site facts:
[the engine map](../exe-reverse-engineering.md#the-cob-modules-entries-every-start-outside-a-slice-and-the-script-slice-disassembled-2026-10-12).

**Fields we write:** native protocol initialization writes extended start length
`0x512AF8`, identified-text length `0x512B84`, callback `0x512ACC` and mask
`0x512C6C`. An accepted 97-byte identified text delivery replaces the first 65
bytes of this pump call's private receive allocation with its validated embedded
stock packet before entering the stock text case. The receive redirect writes
only the pump's stack message pointer; a nested pump restores its parent's
`main+0x4C9/+0x4CD` transport metadata before resuming it. Capture reads the unit array with each unit's mover and order lists, the ten
player records with their PlayerInfo, the controlled and viewed players, the unit types, the
sort buckets, GameTime, the game mode and the map's path, the feature grid, the wreck pool, its
heads and the reproduction cursor, the feature defs, and at the create's exit the new unit's water
state `+0x10A` (the create writes none of it), each bounded by the engine's own count
([the engine map](../exe-reverse-engineering.md#the-demo-recorders-capture-where-it-runs-and-what-it-reads-disassembled-measured-2026-09-29)),
and every `SIM_STATE` table Impure declares; and, for S3b, every field of the positive list
`replay-globals.tsv` (main-block offsets checked against the block's `0x3924D` bytes and `.data`
addresses against `0x501000..0x52B65C` by its generator), the projectile pool's count and its
records up to their written extent, bounded by the pool's size, and the explored map
`*(main+0x14273)`, sized from the grid
([the engine map](../exe-reverse-engineering.md#the-demo-recorders-globals-projectiles-and-seek-disassembled-measured-2026-10-01)).
A dump also reads every player's sight counters (`[rec+0x7C]`, `[rec+0x88]` bytes) and the main
block and `.data` whole, and carries the counts (`RK_COUNTS`, `RK_COUNT_NAME`), Impure's DLL's
base and `SizeOfImage` (`RT_DLL`) and each recorded table's layout (`RT_FIELDS`). It no longer reads `tagpu_replay.trigger` itself: the replay's frame visit
does, and arms the dump (§2.6f). The writer thread reads no engine memory, and since
`tagpu_recwriter.c` is not on `thread-split.allow` the build refuses an engine address in it.

**Three simulation tables moved to the heap for it**, so their pages are write-watched: the build
ghost's memo `s_ghost` (`tagpu_datakeys.c`) and the transported explosions' marks `s_txMark` and
`s_txKillMark` (`tagpu_patches.c`). Each is allocated when its module installs and never freed.
If the memo cannot be allocated, the ghost masks are not kept and every ghost shows every piece.
If a mark table cannot be, the transported explosions fail the way a stub that cannot be made
does: the fail-closed table writes none of its rows, and the process ends with its failure report
at the first DirectDraw call (`limits: FAILED`). The paths that do not rest on the memo or the
marks being there — the ghost mask, B4's refused-kill receiver, the level's reset — test for NULL.

**The coverage build** (S3c-3; `make COVER=1`, `ddraw-cover.dll` with its linker map
`ddraw-cover.map`, and `make LIMITS=stock COVER=1` for the stock-limits twin) installs no hook and
writes no engine memory. The `sim` files of `thread-split.allow` and the recorder's own four are
compiled with `-finstrument-functions-once -ffunction-sections`, so each of their functions calls
`__cyg_profile_func_enter` the first time it runs; `tagpu_cover.c` marks that function's first
byte in a bitmap over the DLL's `.text`, bounded by the section header (an address outside it
marks nothing). `cover <name>` in `tagpu_replay.trigger` writes `replays\cover-<name>.bin` (`ICV1`,
the count, each function's RVA) at once, and the first name armed is written again at the level's
end (`tagpu_rec_level_end`). Any other build refuses the line (`cover: <name> refused: not a
coverage build`).

**Multiplayer contributions:** `tagpu_recmp.c` carries contributions on one worker's distinct native TCP/IP application/session, and `tagpu_recmp_orders.c` caches authoritative human/hosted-AI queues without following remote list objects. Native observed-world files are written on request, and the puppet host plays, seeks and takes them over through the owner-authority materializer (§2.6f). The host's start call `0x496D25` carries a fresh match ID; observer `0x4553BE` retains the original start action and initializer `0x4520BE` writes its 25-byte length. Native CreatePlayer at `0x4CA762` appends a public key to the original 21-byte prefix; system size branch `0x454315` accepts 21 or 301 bytes. All five sites are checked and made writable before any changes. B3's `tagpu_wire_received` supplies the start action's actual delivered span. The primary Receive wrapper is stock. Details: [multiplayer slice](../tadr-port/demo-recorder-multiplayer-slice.md).

The game thread copies the native host address and bounded actor roster/public keys/ownership. The worker retains no primary interface or engine pointer. `tagpu_recmp_net.c` owns sixteen native async completion contexts, while signed bindings freeze each primary human/AI origin's private session, stream and actual SDK sender. Data waits in bounded storage until every expected actor binds and identity announcements precede data on the private sender. A full transmit queue ends contributions incomplete, as unbound (`RECMP_UNBOUND`, naming the primary IDs that never bound) when an expected actor has not bound; the game never joins the worker. A private session open that loses the TCP/IP provider's listener port to another peer is opened again, at most ten times, and a send SendEx answers with DP_OK keeps its context until its completion as a pending one does. A remote packet the game cannot apply, a full receive queue, or a stop with no recording attached ends only this peer's consumption (`tagpu_recmp_relinquish`): its own recording ends incomplete, while its sends, seals, final seals and receipts continue. Details: [channel failure handling](../tadr-port/demo-recorder-multiplayer-slice.md#channel-failure-handling).

The native fixture delivers sixteen DATA packets per peer through the production channel initialized after both primary interfaces close. Actual TA also verifies the isolated channel with two humans and one hosted AI each: both bind their two remote actors and receive resources and nonempty queues without loss/failure. These tests do not establish terminal completeness, physical bandwidth bounds or a network file. The engine map records why the viewed-row `0x4655A1` resource send cannot advertise every idle AI; no resource-message patch is installed.

Source-prefix seals (`tagpu_recmp_stream.c`) validate contiguous sequences, closed source ticks, cumulative DATA bytes and CRC before publication. B2 closes the previous capture tick only when its clock advances; a final seal freezes a local producer, while capture failure aborts without success. The game consumer refuses a seal with a partial order transaction, and relinquishes consumption. Native fixture peers receive exact final human/AI prefixes after primary Close; a withheld last DATA fails at its valid final seal. Both live TA workers also finish after ordinary peer exits. The earlier exit test leaves two markers unread on the joiner after its game consumer stops. A vanished unsealed peer prevents transmission retirement; the drain deadline fails incomplete. Frozen per-source consumption cuts now require covering seals, and authenticated 48-byte final receipts validate against the worker's accepted marker. A retained sole consumer can drain late packets after end without moving those cuts. Active-source, withheld receipt/cut and altered receipt controls pass; the native fixture validates four receipts per peer. Actual TA verifies the departing host closes with cuts 2/2 and four receipts while the joiner remains in play, then the joiner exits cleanly with cuts 2/2. Game capture now attaches a file context before the first publication; engine authority application remains open.

An optional engine-free source/application journal now copies local publications and remote consumptions into one bounded 16 MiB SPSC stream. It preserves source envelopes, assigns a global consumption/production ordinal and retains application ticks separately. A failed local journal detaches while mandatory contributions continue. `RecRing` can transfer its copied context to the actual writer, which writes contribution blocks and a validated cut footer; the readers read recording format 26 only. The synthetic `.imprec` fixture passes normal closure, C/Python reading, world seeking, missing-proof/local-save failure and semantic corruption controls. Game capture attaches that context on local recording opt-in, before the first publication/consumption and writer start. Full owner authority, engine authority application and network playback remain open. No new engine address, patch or engine write is added by the container.

The production adapter's resource-only samples are now full normalized 520-byte human/hosted-AI player images and changed-word deltas (`tagpu_recmp_players.c`). A copied roster freezes each primary origin's recorder-local seat and bounded fixed unit block; remote queues use it for authorization. Complete player updates preserve source stream/tick/sequence and actual application tick, while unknown or malformed input cannot publish a replacement. The player-cache/journal fixture reconstructs all four exact source images in C and Python and passes roster/provenance/guard-page controls. Actual TA with two humans and one AI hosted by each verifies all four first full-image CRCs/source stamps, nonempty queues through frozen blocks and successful cuts/receipts at ordinary exit. The optional file context now copies the roster into `RT_MP_SEATS`; writer and C/Python readers require exact source/world/seat ownership agreement. The synthetic container passes 23 malformed-mapping refusals, including file-info validation when mode is falsified. This does not close remaining owner-only effects, declared Impure state, takeover or game network playback integration. The adapter reuses existing B2/player-image reads and writes only its owned caches; no new patch or engine field write.

The file writer and reader validate semantic player/unit/order history (`tagpu_recmp_history.c`). Bounded source/application cache groups keep partial updates and proof tails from replacing the last complete recorded image/list/absence. Reader queries reconstruct authority at the current world tick after playback/seek; returned images remain separate from `RecWorld` and engine application. The controlled 3,108-byte file reconstructs four synthetic full players, two mapped AI unit/mover images, explicit absent slots and an exact nonempty list, preserving application authority across partial/proof updates. All five lifecycles and 43 body refusals whose raw source seals/cuts remain valid pass.

The copied-image player-row mapper validates source/destination origin sets and global blocks, permutes five player-indexed vectors and the self row, and preserves every other byte. Its isolated C fixture passes 640 roster permutations, 3,520 exact round trips, unchanged-output refusals and guarded spans. Wire-version-2 source kind 7 announces each copied frozen roster at sequence 1. The game adapter validates local/remote images against it; file history keeps source/applied maps separate and reader player queries use recorder coordinates with original provenance. C and Python verify every vector entry, including vacant/dummy rows, and proof-only maps establish no applied authority. The native four-actor rerun verifies first-roster/full-image sequences, actual human row swaps and ordinary-exit cuts/receipts. An earlier private-session SDK join timeout remains unexplained; the adapter names its failure stage. No engine field write or patch is added.

`tagpu_recmp_units.c` now bounds/maps owner/resource-player IDs and attacker/owner rows in copied 328-byte unit/mover images. Kind 8 carries full/changed-word images and explicit absence with birth/source/application provenance. Its game-thread cache validates the entire batch and source/cache allocation agreement before publishing any slot. The isolated fixture passes 464,640 exact inverse mappings over 640 rosters, atomic refusal and guard-page controls, 201-entry batches and final-slot bounds at 65,531 slots. Actual native TA verifies all four human/hosted-AI first full unit-image source checksums/provenance and mapped human rows, plus ordinary-exit cuts/receipts. C/Python file queries retain unknown-versus-absent and proof-only delta isolation through synthetic seek. The adapter reads bounded frozen local blocks and the existing declared birth table, and writes only owned caches. Engine application, COB/model/order-owned objects, effects/projectiles, AI planner, complete declared Impure authority and native engine playback/seek remain open. The engine map documents create/resource/damage/death and lookup helpers read for this audit, including the uncalled helper and unbounded initializer.

Owner constructor begins/creates, deaths/returns, script starts, replies and nano sprays share one copied kind-10
carrier and 4,096-call game-thread queue (`tagpu_recmp_roots.c`), independently
of the recording switch. The existing fifteen start observers, extra-weapons
helper, constructor reservation and successful exit, destructor entry/return, two reply observers, nano query/dispatch credit and two spray observers
supply unchanged root payloads. Mixed kinds and contiguous actor switches retain
call order, source clock, incarnation and pre/post-B2 phase. Old calls drain before
source seals and final calls before teardown seals. Whole-batch bounds validate
frozen ownership, names/arguments, 24 reply kinds and ten signed spray layers
before publication; an uncredited multiplayer spray refuses contributions.
The constructor-begin observer byte-matches nine bytes at `0x486075`, after
B4 fixes the selected slot/birth and before initialization or child roots. It
copies the same 28-byte arguments into distinct root `0x100`; the ordinary
creation event remains at the successful return. The span has no relative
instruction, since the generic observer copies without relocation. No engine
field is written. The worker sees copied packets only.
The legacy kind-9 cache remains for reading earlier experimental contributions.
Death entry copies the normalized victim/mover image while this same-thread
destructor still owns it. Return publishes its saved source/birth identity without
reading the freed mover or cleared slot. Frozen source-owner rows, killer IDs,
player indices and the live image validate before publication. These brackets
also run with recording off; frame/queue exhaustion makes capture incomplete.
Codec/queue controls pass 83 atomic refusals, 1,300 guarded batch truncations,
485 guarded per-kind short spans, maximum 167 replies/251 returns and 8,392
ordered mixed calls, including source owner-row authorization and every
constructor/finished/position bit. The synthetic C/Python file verifies six mixed
applied batches and per-origin exploration through seek, proof-only isolation,
five lifecycles, 23 header and 135 body refusals. Queries return copied authority
separately from `RecWorld`. The current four-actor native coverage fixture
matches all 28 first root observations between source, recipient and saved file;
71 exact constructor begin/success pairs bracket eight private reply calls.
Its complete 2,845,687-byte recording ends at tick 558 with zero corrections.
C/Python retain all 8,146 applications, 2,061,238 bytes and CRC `8EA54D8E`
through reader world seek; both peers reach the new boundary/payload helpers.
All 23 shared `secondary-orders` skirmish checks pass at internal speed 20,
including three exact poisoned round trips. The native run observes speed 10;
native engine play/seek and complete authority remain unfinished.
`tagpu_recmp_lifecycle.c` consumes that ordered prefix into normalized owner
lifetime actions. Whole-batch incarnation/type/bracket validation precedes every
callback; initial full images bootstrap units, proof-only images do not. A failed
callback stops the consumer permanently. Against the saved four-actor file,
independent C/Python audits retain 10,372,503 action bytes/CRC `8CF737F2`, all
6,000 owned slots, 71 construction and 24 destruction pairs, eight constructor
replies and five script starts during destruction through fresh reader seek.
This adds no hook or engine field write. Native construction/destruction and
suppression of regenerated child roots remain the adapter's open work; saved
network engine playback is still refused.
The allocator ownership boundary at `0x48587E` now orders the ten player pointers
by frozen global actor blocks before the native initializer writes their paired
fields. Copied input validates before any stack write; other levels remain inert.
A relocated retail-allocator fixture passes 206 layouts, two untouched native
ordering controls and all ten paired blocks. The production hook is installed
through all four coverage skirmish processes; all 23 replay checks pass at internal
speed 20. Multiplayer loader admission and engine reconstruction remain open.
`recr_mp_apply` visits every application envelope in recorded ordinal order
through the current world tick, retaining both clocks and the full copied batch.
Its separate cursor prevents copied-state queries from consuming playback data.
All 30 envelopes match independent Python bytes/provenance; A/B/A batches,
resumed ordinals, repeated polls and backward/forward seek preserve the sequence.
The engine map corrects the notice-table stride to 24 bytes and documents both
reply viewer filters and spray's signed low layer index. Engine application,
independent clock mapping, remaining roots/Impure authority and native engine
playback/seek remain open. Native normal-speed four-actor TA with recording off
verifies all twelve first script/reply/spray observations against recipient bytes,
source metadata, event index, incarnation and phase, plus ordinary-exit cuts and
receipts. Only post-B2 calls are observed in that native sample. This is a worktree
checkpoint. The current coverage build passes all 23 skirmish `secondary-orders`
checks, including the shared constructor observer, puppet playback, restore,
fresh seek, inventory and three exact poisoned round trips. No exclusion or
comparison rule is changed.

Current native COVER verifies all 24 first root-kind observations across four
human/hosted-AI actors, with host recording and joiner off. The complete
2,432,312-byte file (ticks 1..468, zero corrections) has 22 matched owner
destructor entry/return pairs; five local primary death images match the copied
payloads exactly. Both peers reach the entry, return and new saved-identity
helpers. C/Python retain all 5,994 application envelopes and 1,054 root batches
through fresh world seek. The applied calls observe both B2 phases; the death
pairs in this sample are post-B2. All 23 skirmish replay checks still pass at 2x,
including three exact poisoned round trips. Separate constructor-arrival analysis
finds two later Reclaiming replies before the recorder's replica exists. Owner
timing was chosen by the owner in product decision 7: unit creation/death follows
its owner and private roots keep recorded application order. The owner-authority
materializer (§2.6f) applies each unit's images to that exact incarnation only.
The engine map records the unbounded native constructor inputs, its occupied-slot
death, and why unfinished construction plus B4 carried state differs from a
finished local constructor. These reads install no network constructor patch.

Current COVER native capture also verifies all sixteen first constructor/script/
reply/spray batch observations with one peer's recording off. Seventy exact
constructor tokens join their carried live source type/owner; all 35 primary
local constructor payloads equal the contribution bytes. Eight other roots
precede their constructor's success exit. The complete 1,401,962-byte recording
retains 3,052 ordered applications and 508 root batches through C/Python fresh
world seek, with zero corrections; coverage reaches `ev_create` on both peers.
These dependencies remain separate from observed-world authority. Native engine
materialization/playback and the complete multiplayer landing gate remain open.

Native `mp_record_begin` now transfers `RecMpFile` into the writer before any
source publication or consumption. Mode-3 headers retain copied transport and
seat/block metadata. The primary stream captures this peer's observed world and
local queues; controller 3/4 list heads are skipped before traversal, with exact
owner queues preserved separately in the contribution ledger. Script/reply/spray
owner batches have no duplicate primary-stream events. Native capture keeps the
skirmish fire lever false and excludes live rewind. The producer clears its file
borrow before publishing context end; local-save failure detaches the journal
without ending mandatory contributions, while channel teardown permits sole
proof-consumer transfer. No new engine field write, patch or exclusion is added.

A native normal-DLL four-actor game, host recording enabled and joiner disabled,
produces a complete 2,524,535-byte file at GameTimes 1–536. The writer reports zero
corrections. C and independent Python validate 6,697 ordered applications,
1,673,880 application bytes, 1,073 root batches, all four players and unit-block
bounds through sequential reading and world seek. Source/recipient roster/player
and first script/reply/spray bytes match; ordinary host exit validates cuts 2/2
and four receipts before the joiner exits. Actual native speed is 10. This is
observed-world capture and copied authority; playback through the materializer
(§2.6f), the perspectives, chat and ten seats came later, and the physical traffic
budget and host migration remain open. See the multiplayer
slice's *Native observed-world file capture* for exact conditions.
The native file also stores replica creation roots from B4's existing success
callout at `0x48634F`, after the incarnation and carried build/HP state are set.
The 32-byte `RK_EV_NET_CREATE` retains constructor arguments, owner, water,
birth and post-B4 unfinished fraction/HP. No byte patch or engine field write is
added. C/Python readers bound the root to its frozen owner block/type/enum and
fraction before exposing it; format-5 reading remains supported. The writer's
3,554-byte fixture passes exact preservation, fifteen malformed-root controls
and the existing contribution/seek/lifecycle checks. This supplies capture data;
native engine playback and causal ordering of early owner roots remain open.
An actual normal-build native recording verifies all 35 replica roots against
the next B2 type/owner/birth/build/HP images, with identical C/Python root bytes
through seek; all 353 observed-world transitions are explained. The complete
format-6 file ends at tick 354 with zero corrections and 4,086 ordered authority
applications. A coverage attempt fails private session creation (`80004005`)
and ends incomplete at tick 2; successful native coverage remains required.
A native local-save failure at tick 1 also leaves all four actors contributing
and ordinary cuts/receipts valid. The same process then records a complete
mode-2 skirmish to tick 1,293 with zero corrections, 908 shots, 1,985 script roots
and no contribution blocks at speed 20. A separate coverage-build attempt fails
private SDK enumeration with `80004005` before binding; it remains a startup
reliability gap, alongside the earlier join timeout.
The recorder also captures each controlling human/hosted AI's explored-map
bit history as kind-11 origin-keyed full/delta bitmap transactions, retaining
the complete LosType and source/application stamps. B2 bounds the raw u16
reads by LoadMap's nonwrapping allocation geometry and uses game-thread level
lifetime. The copied codec publishes only END, source seals refuse partial
transactions, and file readers keep proof-only source state separate from
applied exploration. Codec and controlled file/seek checks pass. A native COVER
sample verifies all four first source/recipient bitmap CRCs, but its exit/chat
driver fails; the complete native map file/mode/seek gate remains open after
three attempts. The current `secondary-orders` replay has 22 passes and one
images-inventory failure at three bytes of `main+0x531`; all simulation
comparisons and three exact poisoned round trips pass. `main+0x531` is the `leave` row `guitop` in `replay-globals.tsv` (the owner's decision, 2026-10-06), and `secondary-orders --cover` then passes all 25 checks, the images gate included (MEASURED 2026-10-06). This adds no hook, field write or exclusion. Effective POV
engine playback, other view streams and communication audiences remain open;
see the multiplayer slice's *Per-origin explored-map history*.

The native text path also carries `(fresh match GUID, native sender, 64-bit
serial)` beside the original 65-byte text packet. Source GUI scope/format/send
call-site replacements at `0x493FAF`, `0x463E9F`, `0x4533F9`, `0x45348B` and
`0x4534B6` preserve the original audience logic. The new `0x2B` dispatch slot
`0x456028` validates the exact 97-byte delivery and bounded actor identities,
then enters stock's text case at `0x45522E`. It preserves the pump pointer and
copies no engine address to a worker. Normal/COVER builds and 168 copied-packet
guarded/malformed controls pass. Intended targets and actual recipient
observations remain separate; playback,
loading-phase acceptance and pings/drawings remain open. The first native wire
probe fails input acknowledgement and exposes an inactive start-token guard;
the corrected in-play scope replaced that guard. No passing native wire-ID
gate is claimed from that attempt. The corrected COVER three-human run, local
recording off, verifies five unique source events and exact packet CRCs: the
selected recipient accepts all five; the other peer accepts four, excluding the
private event. Repeated identical lines remain distinct, and a full-length line
has equal 63-character displays on every peer. This is in-play queued native
delivery; the open playback/loading/raw gates remain.
The engine map and multiplayer slice record the invariants and verification.

Kind 12 persists text sources and actual receipt observations, including the
original tick, recipient-owned clock, recorder application tick/order, intended
target-call IDs, display flags and exact stock packet bytes. A fixed identity
join validates all 65 bytes even when a receipt precedes its original. Targeted
intent bounds actual receipts. Exhaustion of 65,536 identities or the 4,096
copied queue refuses contributions; it never truncates an event history.
Root/text queues drain in tick order before the shared clock advances or seals.
Writer and C/Python semantic histories validate proof-only originals without
publishing them, and a complete end refuses a receipt whose original is absent.
`recr_mp_text` inspects the latest applied batch; all batches remain in the
ordered application visitor. Copied controls and the existing file regression
suite pass. A COVER three-human match stores five originals and nine actual
receipts in a complete native recording with zero corrections; saving is off
on both recipients. Public repeats, the sampled private recipient and a
full-length line retain exact wire CRCs and separate clocks. This does not
enable multiplayer replay display or close loading/raw transport/POV/budget
gates. The multiplayer slice records the current native reader verification.
An independent three-human COVER run also retains private originals from two
saving-off authors, both actual receipts and a public marker: seven entries,
three distinct author/serial-1 identities. The recording host has neither
private line in its sampled display and no private receipt. The complete file
has zero corrections and all 324 application envelopes / 160,587 bytes remain
exact through C/Python reader seek. The reusable saved-evidence fixture passes
both this recording and the fourteen-entry repeated/full-length recording.
The loading audit identifies state-5 deliveries before B2 and two loading
pumps sharing the retail receive allocation. The recorder redirects the
whole primary pump at `0x453D40` and its receive call at `0x453D94`: one persistent
storage slot per recursion depth survives return and is reused across calls,
threads and levels. A recursive critical section serializes native
receive/dispatch metadata; busy admission returns 0 without waiting or consuming
an SDK message. Nested calls retain independent
storage and restore their parent's source/destination words. Native sends
continue using their own shared output buffer; input never borrows it. A full
u16 allocation bounds the unchecked queued copy; raw/system messages negotiate
larger sizes before publication. The native three-human COVER run retains
correct private/public audiences and closes at tick 709 with zero corrections;
seven text entries agree through C/Python reader seek. A separate raw run
closes at tick 799 with zero corrections and seven exact entries through reader
seek. Exact-core controls cover maximum queued size, raw growth, nested
lifetime/metadata/span, refusal, two-thread receive ordering and the actual
guarded movement copy. That copy now uses the shared receive-span API; native
queued and raw movement tests observe both commanders on both peers with no stale
refusals. Exact-core controls also verify packet aliases after return, depth-slot
reuse and a foreign thread's busy refusal while the owner holds the lock. These
lifetime and admission invariants remove early packet freeing and introduced
lock waits; they do not prove every native parser reentrant, SDK callback/cost
behaviour or separate output-builder lifetime. The installed 32-byte wrapper
also passes queued/raw movement fixtures after those changes, with positive
movement counts and every sampled stale count zero.
The identified mask is now 7, with the adapter restricted to loading/play
states 5/6. A fixed process-owned inbox publishes copied match metadata and
4,096 bounded observations from either pump caller to the first B2 producer.
Its held bodies acquire no other lock and make no engine/SDK/log/allocation
call. Acceptance clocks are copied inside publication ownership; the frozen
roster bounds IDs before transfer. Pending start survives load reset and is
claimed once. B2 drains before source advance. Teardown transfers final entries
and closes admission under one ownership interval before final seals/queue
free. Exact inbox
and state-5 receiver controls pass; native loading delivery/clock/file gates
remain open. Whole-game receive
cost, bandwidth and continued migration remain open.
The snapshot also publishes inline `edit_index`/`caret` without engine writes.
A two-peer COVER run verifies acknowledged text replacement and the engine's
Mapping toggle. This is driver verification, not a native replay gate.

| Fields we write in the multiplayer checkpoint | Invariant |
|---|---|
| `0x486075..0x48607D` constructor reservation code | One whole-span-matched observer jump; its stub preserves registers/flags and runs the original two nonrelative argument loads. Reads only bounded fixed-array owner/water fields and the already assigned exact birth, publishing copied begin arguments before initialization. No engine data field is changed. |
| `0x512AF8` startup packet-length entry | 25, written by the redirected initializer store; start sender, receive observer and initializer are installed together. |
| Host start's message bytes | Fixed 25-byte stack value passed through the existing generic sender; its reliable queue copies before return. No remote engine resources or order fields are written. |
| Native CreatePlayer initial data | The original 21-byte password/version prefix followed by a fixed 280-byte public-key record. Only public material is exported; key failure forwards the stock data and refuses faithful contributions. The size handler retains the original accepted-prefix tests. |
| `0x512B84`, `0x512ACC`, `0x512C6C` | Fixed identified-text size 97, generic `ret 4` callback `0x44FD40` and receive mask 7, written by the same startup initializer; the copied-inbox adapter accepts only states 5/6. All thirteen primary sites are acquired before any is written. |
| Scoped GUI text message | Copied 97-byte stack wire value through the original target/generic sender. The native queue copies before returning; ordinary binary `0x05` messages retain their existing size. |
| `0x453D40`, `0x453D94` / pump stack `+0x10` | Byte-matched entry wrapper and receive-call redirect. A 32-byte stack frame publishes an exact active TLS span; one process-lifetime allocation per recursion depth retains the last packet across return. Nonblocking recursive ownership covers receive/dispatch; busy admission returns 0 and consumes nothing. Success writes the current input pointer to the dispatcher's existing stack slot. All original return registers/flags are preserved. |
| `main+0x4C9`, `main+0x4CD` | Native receive writes source/destination as before. A nested pump restores its parent's saved words before returning to that parent's dispatcher; the process-lifetime main block remains owned. No primary interface is retained. |
| First 65 bytes of this pump's private receive allocation | Exact delivered pointer/97-byte length and copied version/match/serial/actor validation precede the bounded stock-image replacement. The original text case reads that same allocation before the next receive; native sends cannot overwrite it. |

### 2.6f Watching a recording (`tagpu_replay.c`, `tagpu_puppet.c`, `tagpu_recreader.c`, on request)

S2b and S3b of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s2b-watching).
`tacli replay play <instance> <recording> [--perspective RECORD]` asks the next SKIRMISH Start,
through the request file `tagpu_replay.play`, for the recording's map with every recorded player
seated as a type-3 record the recording drives, a puppet, and the viewer in a seat V of their own.
The level then plays the recording: each step, after the host's own unit tick, the replay plays
that step's recorded events through the engine functions that made them and writes the recorded
world of the next. Without `--perspective` it shows everything (V's full visibility); with it, that
recorded player's view, radar picture included. `tacli replay seek <instance> <T>` moves the
running playback to GameTime T, forward or backward, and with `--recording` launches one that
plays from T ([S3b](../tadr-port/demo-recorder-skirmish-slice.md#s3b-the-globals-and-seeking)). The
engine facts are
[the engine map](../exe-reverse-engineering.md#the-puppet-host-its-gates-the-30-tick-deadline-and-the-sight-loop-disassembled-measured-2026-09-30)'s
and, for the seek,
[its S3b section](../exe-reverse-engineering.md#the-demo-recorders-globals-projectiles-and-seek-disassembled-measured-2026-10-01)'s.

**Taking over** ([S4b](../tadr-port/demo-recorder-s4-takeover.md)): `tacli replay takeover <instance>
[<recording>] --at T --player P [--difficulty D]`, or `takeover <P> at <T>[ difficulty <D>]` in the
trigger, runs the seek in its TAKEOVER mode at the next frame visit: the world of T is made as a
seek makes it, but with P human and the other recorded seats AI, and the level goes on as a live
skirmish from T. The recorder then records the game after T as a new timeline appended to the same
file, through the handle the takeover opened. A playing of a forked timeline (`--timeline N`) holds
at each boundary of its chain and crosses it with a seek to the child's first iteration
([engine map](../exe-reverse-engineering.md#the-takeover-from-a-recording-seat-types-the-ais-objects-passability-and-the-order-nodes-disassembled-2026-10-02)).

**Fork files** ([S4c](../tadr-port/demo-recorder-s4-forks.md)): at most three live
forks, eviction by last-used time, explicit delete/export and shared ancestor
prefixes. `tagpu_recfile.c` runs maintenance on tracked workers without engine
reads or engine-field writes. The common game-thread flip observer polls its
`forks` requests at menus too; no new byte patch is installed. A persistent path
lock stays held across closing the source and atomic replacement. The playing
and a queued deletion reserve canonical path, file identity and timeline under one publication
lock; exit waits for recording writers, then seals and waits for maintenance.
The existing **fields we write** below remain those of playback and takeover;
file metadata changes add no engine writes.

**Live rewind** ([S4d](../tadr-port/demo-recorder-s4-rewind.md)): `tacli rewind INSTANCE
open|seek T|now|fork|close [--dump NAME]` pauses a recorded skirmish, preserves its
present while inspecting earlier frames, and either restores that present or ends
the original and appends a fork. The writer acknowledges a complete-prefix flush;
recording remains suspended during reconstruction, preserving pending live events.
The **pause guard at `0x496920`**, stolen `84 98 51 8A 03 00`, observes before the
engine's pause test and reasserts pause while rewind owns the level. Failure to
install it disables live rewind.

The **round-trip batch guard at `0x49692D`**, stolen `A1 E8 1D 51 00`, runs after
the scheduler `0x495230` returns at `0x496928` and before the count test at
`0x496932` and step call at `0x49693E`. The two calls and the count test are
byte-matched too. On a puppet with round trips armed, it bounds the due count by
the nearest upcoming T−1 before `0x495490` caches that count in EBX. This makes
the required frame boundary independent of frame timing. It preserves registers
and flags and changes no iteration's step size. An unavailable guard refuses
round trips; the frame dump and playback sites remain independent. The same
observer carries a test launch's pacing: after `turbo on` (`tagpu_turbo.on`'s k)
it sets the count to k, or to 0 while the recorder's ring is more than half
full, an armed `pause at <T>` bounds it to end at T, and a test launch's hold
(`tagpu_hold.on`) bounds the level's first batch to one tick; the frame visit
then sets the pause bit (*The scheduler, the clock and the test pacing* in the
engine map), which the pause guard at `0x496920` keeps set until `resume`. An
unavailable guard refuses the turbo and the pause too, and leaves the hold off.

The harness's AI activity observation brackets `0x408C40`, stops attribution
at the shared targeting phase `0x4089A0`, samples constructors at `0x43A0C0`,
and forgets destroyed samples at `0x43A1F0`. Existing dispatch observers
`0x43B878`/`0x43BB1D` count execution of those nodes. The three new sites are
byte-matched observers; the existing weapons acquisition wrapper ends planner
attribution without another patch at `0x4089A0`. **Fields we write:** diagnostic storage only; these
observers write no engine field and no recording payload.

The **multiplayer allocator ownership observer at `0x48587E`** checks the whole
nine-byte span `33 FF C6 85 FF 00 00 00 FF` and preserves registers/flags around
its callback. The loader's stack contains ten sorted pointers at `esp+0x18`
and the fresh arena at `esp+0x14`. Only a multiplayer puppet reorders that
validated permutation before the native ownership loop. Saved multiplayer
playback is still refused; the installed observer is inert on existing levels.
This callback runs on the loader thread. Seating publishes a copied immutable
roster before thread creation; interlocked claim/result operations carry it to
the allocator and READY/FAILED back to the first game-thread step after the
engine's load-done gate. The loader reads no mutable replay reader and does not
write its failure flag.

**Sites.** Six call sites of the Start, the loader and the destructor, installed by
`tagpu_replay_init`, all or none, and inert without an accepted request: the destructor's death cue
`0x4869EB` (`call 0x494FF0`, the music's combat ring +5 when a death's killer byte equals the
controlled index), which returns with its argument popped on a puppet level, where the controlled
record is the viewer's record 10; the seat fill's call `0x47B092`, the settings
persist's call `0x47B09C`, the start units' calls `0x497963` and `0x497ACD`, and the level init's
call `0x4919C8` (`E8 C3 2F FD FF`), retargeted to `tagpu_replay_level_players`, which calls
`0x464990` and then, on the loader thread and only on a level the seating armed, the player init
`0x464700` on record 10, the viewer's ([the engine map](../exe-reverse-engineering.md#the-replay-viewers-record-player-record-10-and-the-level-inits-call-0x4919c8-disassembled-2026-10-06)); with them an **observer on the
settings save `0x430F00`** (`tagpu_detour_observe`, stolen `A1 E8 1D 51 00`, a `before` and an
`after`), inert unless a recording's options are in. The teardown wrap's post hook (§2.7) calls
`tagpu_replay_level_after` once `0x491B60` has returned. The step runs from B2's stub at
`0x4954ED` (§2.6c), after the unit tick, and the capture-point write (`tagpu_replay_capture`) first
in that stub, before the recorder's capture and the unit tick. **The frame visit, `0x4969CD`**, the
in-play frame callback's `call 0x468CF0`: checked by `call_is` and rewritten to call a stub
`pushfd; pushad; call tagpu_replay_frame; popad; popfd; jmp 0x468CF0`, so DrawGameScreen runs with
the site's own return address `0x4969D2` and the observers on `0x468CF0` see the frames they saw.
It is installed first and on its own, since a dump of a live recording needs it too, and runs on
paused frames as well, between two sim steps, before the draw; the scenario applier's hook at
`0x4969D2` takes the next instruction's bytes. **Twenty-one prologue gates** (`tagpu_puppet.c`,
`tagpu_detour_gate`), landed at `DllMain` before the recorder's observers, refuse on a puppet level:
the apply `0x489CE0` and `Send_UnitDeath 0x4864B0` for a puppet's unit; COB's SET (at `0x480B26`,
its second instruction), ATTACH `0x481340`, DROP `0x4813B0` and the two replies `0x47F780` and
`0x47F850` for a puppet's unit, outside the replay's own create and reply; and always the meteor
shower `0x437DE0`, typed commands `0x417B50`, the debug keys `0x4956C0`, the resource transfer
`0x464C60` and the save `0x4326B0`; and from the level's first step, outside the replay's own calls,
the feature tick `0x424050` and the six feature functions (`0x423C50`, `0x4246B0`, `0x423550`,
`0x4233A0`, `0x4244B0`, `0x423710`); and while a seek rebuilds the level, the skirmish
elimination `0x47BD70` (stolen `81 EC C8 00 00 00`, `ret 4`), so the rebuild's kills announce no
defeat; and on a multiplayer recording's segment, the state setter `0x48B090` (stolen
`51 8B 44 24 0C`, `ret 8`) for a call that would change a puppet's `+0x10E` by the setter's own
rule, outside the replay's write of the byte from the owner's composed image
(`tagpu_puppet_state_open`, around `book_state` alone), so each change starts its scripts once.
On any other level a gate reads only the mode byte. The
recorder's hooks serve a puppet level too (§2.6e): the six draws answer from the recording, the
explosion stub makes a replayed death's feature calls, and the footprint writers' `after` undoes
their grid writes. A recording plays only when all twenty-one gates and the recorder's hooks are in,
and the teardown wrap too (§2.7), which ends the puppet level before the cascade destroys its
units. **Three count-only observers** (`tagpu_puppet.c`, *THE COB WRITES ON A PUPPET*, installed
after the gates; without them the dumps carry no COB-write counts) on the state setter `0x48B090`
(stolen `51 8B 44 24 0C`, chained onto its gate, so a refused call is not counted), the yard `0x47DAC0` (`53 8B 5C 24 0C`) and the carry link `0x48AAC0`
(`8B 4C 24 04 83 EC 08`) count, on a puppet level outside the replay's own calls, each call
returning to one of COB's five write sites (`0x480B57`, `0x480BF1`, `0x480BB3`, `0x4813AB`,
`0x481427`) on a puppet's unit, the unit bounded as a slot of the array first; the first 16 are
logged. The gates make them zero; the observers prove it. **The step's phases** (§2.6e) call
`tagpu_replay_phase` after the host's projectile tick and after its explosion tick. **The script
slice** (§2.6e) asks `tagpu_replay_slice_defer` in the host's unit tick: a unit the iteration's
markers name is skipped there, and the replay gives its slice at the marker in the step's events,
at a multiplayer own create's CREATE_END, or, counted and noted, at the step's end
(`tagpu_replay.c`, THE SCRIPT SLICES). **A replayed death's Killed** runs as the recording
machine ran it: the destructor's run-now start at `0x486877` goes through `dtor_killed`, which
inside a death the recording machine made itself makes `Send_UnitDeath`'s query `0x4B0BC0`
instead (`tagpu_patches.c`, THE KILLED SCRIPT OF A REPLAYED DEATH). **The owner's queries**
(`RK_EV_QUERY`) are made at their place in the step's events through the same `0x4B0BC0`, on the
`+0x9A` COB object of a unit live in its slot, with the engine call's arguments
(`tagpu_replay.c`, THE OWNER'S QUERIES).

**Fields we write**, on the game thread during playback, takeover or live rewind,
and on the loader thread at the allocator ownership boundary and the level init's call:

- multiplayer allocation: only the loader's ten-pointer stack table at the
  `0x48587E` boundary. The unchanged native loop then initializes all player
  `+0x67/+0x6B/+0x6F/+0x71` and unit `+0x96/+0xFF/+0xAC` ownership pairs.
  No live unit, arena pointer, slot ID or native transport ID is changed;

- armed round trips on a puppet: the scheduler's due-tick count at
  `main+0x38A3B`, validated within 0..5 and only reduced to the next T−1
  boundary. This pacing word is already `leave` in `replay-globals.tsv`;

- a test launch's turbo and `pause at <T>`: the same count set to k (1..5) or
  0, then reduced to end at T; the pause bit `main+0x38A51` bit 0 set at the
  frame visit after step T and cleared by `resume`;

- the level init's call, on a puppet level: record 10's buffers through `0x464700`; when they
  cannot be made, `main+0x2A42` and `main+0x2A43` = the lowest recorded seat;

- live rewind: pause bit 0 at `main+0x38A51`; the spectator's seat (record 10) and buffers
  through `0x464290` and `0x464700`; recorded seats' type through `0x463C60`,
  acquisition table through `0x40B390`, and AI `+0x74` cleared only after all their
  units have been destroyed. V gets the fresh loader's WinLoseTime `+0xF4 = 0`,
  PlayerInfo `+0x9B` count nibble and loaded bit, and `+0xA7/+0xA8` version bytes
  from `main+1/+2`. Return-to-now restores unit contact bits `+0x110 & 0x1700`
  from each bounded, identity-matched saved unit instead of running an extra radar scan;
  it restores the saved viewed player,
  the 2160-byte message area at `main+0x12EF` and its two indices at `+0x2A3E`,
  with the saved controllers supplied to the takeover. No difficulty change is
  applied on that return;

- at the seat fill: each seat's record through `0x464290(seat, 3)`, then PlayerInfo `+0x95` and
  `+0x96`, the name at `+0x2B` and `+0x49`, and the alliance row `+0x108..+0x112`; V's, record
  10's, through `0x464290(10, 3)`, with PlayerInfo `+0x9B` bit 6 (the watcher bit) and the
  recording viewer's side at `+0x95`, the empty block `+0x67` = 1, `+0x6B` = 0 and the ID range
  `+0x6F` = `+0x71` = 1 (the whole record kept beforehand, and put back after the teardown with
  `main+0x2A42`/`+0x2A43` = 0 when either names it); `0x464290(r, 0)` for a record the stock fill activated that the recording does
  not seat; `main+0x2A42` = V, `main+0x2A43` = the view, `main+0x2A3C` = the recorded player
  count; the objective disarmed (`0x4904B0`);
- the skirmish settings (`+0x108..+0x11B`), `main+0x37EEE` and `main+0x37EEC`, which the
  recording's options replace after `0x430F00` has persisted the player's; the teardown puts the
  player's back. Every save from the level's first step exchanges the two on entry and again on
  return, so the registry receives the player's; after a save before it, which no known path
  makes, the teardown saves once more through `0x430F00`;
- each step: every unit's record and mover under `replay-layout.tsv`'s classes, positions through
  `0x48A9F0` with `0x43DA70` and `0x43DB50`, carry links through `0x48AB70`, order lists through
  `0x43A0C0`, `0x43A1F0`, `0x43AD10` and `0x489690`, a yard's change through `0x47C790` and
  `0x440A40`; every player record 0–9 under the same classes, but `+0xF0`, the 30-tick
  deadline the host's players phase keeps, and only with both names ended inside their fields and
  a seat's PlayerInfo side and colour within this game's counts; and each seat's `+0x18` = GameTime;
- on a multiplayer segment, with each unit's image: its model's composite frame `Object3do+0x10`
  (`[unit+0x9E]+0x10`) = 0 when the composed image changes whether its build is finished (the
  model the host's create made, freed only by its destructor on this thread; a NULL model
  skipped), and `main+0x37EBE` bit 4 when that finishes a unit the viewer has selected
  (`tagpu_replay.c`, *THE BUILD FINISH ON A MULTIPLAYER LEVEL*);
- the events, through `0x485F50`, C4's death wrap, `0x49D270`, `0x4B0B00`, `0x4B0A70`,
  `0x4B0940`, `0x41B8D0` (not on a multiplayer segment), `0x48B090`, `0x47F780`, `0x47F850`,
  `0x4720D0` and `0x472200`; and a marked unit's script slice, `DoScriptsNow 0x4B0D60` with `dt`
  1 on its `+0x9A` COB object, at its marker instead of in the host's unit tick (the one slice the
  unit tick would have given it, moved, never one more);
- the features' changes, through the feature tick and the six feature functions with the
  recorded arguments, bounded by the level's grid, defs and weapons, each at the phase of the step
  the recording machine made it in: a unit-tick root at the replay step, a projectile-tick root
  once the host's projectile tick has returned, a later one once its explosion tick has
  returned, each followed by the events its bracket holds (`tagpu_replay_phase`, held in a store
  of 8,192 events and 2 MB; past it the held roots and the step's remaining ones go in at the
  replay step, in recorded order, and the step's feature images with them); the draws those calls make
  at the six RNG calls answered from the recording instead of `0x4B6C30`; and a replayed death's
  explosion's calls made inside the host's destructor, from the recorder's stub at `0x486D50`;
- each step, before the host's projectile tick: the next image's cells' unit slots, each only
  when it names a unit alive on the host, since the projectile's collision test reads them; their
  previous values put back just before the feature tick's root, whose reproduction test reads
  them (`0x424189`) and which the recording machine ran on the iteration's slots;
- each step, after its late phase's roots (at the replay step for an iteration played late):
  every cell of the feature grid and every record of the wreck pool the recording
  changed, values only under `replay-layout.tsv`'s classes (a cell's def, record and anchor links,
  and a record's links, anchor, def, flags and sequences are compared, never written; a cell's
  unit slots go in only when each names a unit alive on the host), and the reproduction cursor `main+0x14257` ([the engine map](../exe-reverse-engineering.md#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30));
- at every return of the stamp `0x47CC30`, the clear `0x47D0E0` and the re-claim `0x47C790`, the
  unit slots `+0x00`, `+0x02` and the yard bit `+0x0C` bit 1 of the unit's footprint cells, a cell
  wider each way, put back as they were at the call's entry (`tagpu_puppet.c`, *The footprint
  writers*);
- each step, a unit's control group `+0xAC` through the engine's setter `0x480250(unit, g)`, only
  for −1 or a group below 10;
- at every capture point and at the frame visit after a step, from the recorded iteration the
  host is at: LosType bits 0–2 of `main+0x14281`, only when they differ, with every temporary
  sight record's `+0x1C` zeroed and the expiry `0x482130` first (the replay's own step: the
  engine's LosType commands leave the records alone) and `0x4816A0` after, as the engine's commands
  pair it with the bits; every `write` row of `replay-globals.tsv` (the wind, the game end's
  flags but bits 2 and 4, the simulation RNG `0x51FC88`, the meteors, the pathfinder's table, the
  movement classes' watermarks, `0x51E53C`, `0x51E684`, `0x51E688`, `0x51E6C4`), never the `hold`
  rows (the countdown `main+0x39239`, flags bits 2 and 4); the projectile count `main+0x141F3` and
  the records up to the recording's written extent, their IDs resolved and bounded, and zeros up to
  the host's own extent (a refused write empties the pool and clears `main+0x142F7` when it points
  into it); and the explored map with V's bit kept set, LosType bit 3 cleared when it changed;
  and, before the `write` rows, each live unit's water state `+0x10A` (bounded by its enum) and
  sight stamp's fields `+0x7A`, `+0x7C`, `+0xF8` (through the removal `0x482090` and the stamp
  `0x482270`, a circle's radius index bounded by the frame table's count) where they differ from
  the recording's (`books_put`); on a multiplayer recording's segment also each live unit's state
  byte `+0x10E` from its owner's composed image, through `0x48B090` with the state gate open, where
  it differs (`book_state`);
- at each create: the new unit's water state `+0x10A`, from the create event, bounded by its enum;
- after each step's images: the cells the feature tick's input wrote, again from the next
  iteration's world through the same cell write (the whole grid after an overflow);
- at the first frame visit or step: V's sight counters (`[rec+0x7C]`, `[rec+0x88]` bytes of 1),
  V's bit (`[V+0x146]`, 10) in every entry of the explored map `[main+0x14273]`, `main+0x14281`
  bit 3 cleared, then `main+0x142F1` bit 2 and the minimap's fog composite `0x466C20` (also after
  each LosType change and at a seek), and V's `+0xF0` = the next multiple of 30; and, at the first
  step, `s_windState` seeded from the recording's first image;
- while record 10 is viewed on a puppet level, once a GameTime at the frame visit: the minimap
  rebuild `0x466DC0`, and when record 10's `+0xF0` is due, `+0xF0` = the next multiple of 30, the
  radar pass `0x467440` and the fog composite `0x466C20` (the players phase's viewed-record work,
  which its loop over records 0–9 never reaches);
- at a seek, on the frame visit, in this order: the projectile count 0 (and `main+0x142F7` when it
  points into the pool); every unit killed through C4's wrap of the destructor `0x4866D0` with
  severity and corpse 0; every temporary sight record's `+0x1C` zeroed, then `0x482130`; LosType
  bits 0–2 and `0x4816A0`; GameTime `main+0x38A47` = T − 1; every unit of iteration T made through
  `0x485F50` and written by the appliers above; each unit's water state, its state byte through the
  setter `0x48B090` and its sight stamp through `0x482090` and `0x482270`, from T's image; each unit's bucket through the unlink `0x47CB00`
  (`+0x82` then NULL) and the move `0x47CB60`, and a carried unit's `+0x82`; every cell of the
  feature grid and every wreck record whole, the three heads and the cursor, the host's 3DO model
  instances freed through `0x45AAA0` and made through `0x45A8D0` into `+0x04`; the particles
  destroyed through their own `[vt+0](../1)` and removed from the ten layers, the explosion count
  zeroed, the debris records re-initialised and the flying-piece slots zeroed
  (`tagpu_effects_clear`, both builds); the declared tables and the sim files' restores; the
  globals, the pool and the explored map as above; the viewed record's temporary sight records made
  again through the engine's add `0x482910`, one for each recorded death of a live viewed-seat unit
  under true LOS whose 60 ticks a playing still holds, the reader having started at least 123
  iterations before T to find them; V's sight with the minimap's composite; the viewed record's
  radar pass `0x467440` once (contact bits `+0x110` 8–10 and 12), then, when the host views the
  recording's viewed record, bits 8–10 from the primary stream's images; the records' expiry `0x482130`
  once; every seat's `+0xF0` from the image
  and V's to the next multiple of 30, every record's `+0xF8` = GameTime, the notices `0x47EE30` and
  the chat lines `0x463C80`; and every seat's `+0x18`;
- at a takeover, inside the seek and in its order: viewed = P and the difficulty `main+0x37EEE`
  (the options put the player's back at the level's end); each recorded seat's type through
  `0x463C60` (P 1, the others 2), before R7's creates and again after the players' images; each
  seat's AI object (`0x4B4F10(0x3D)`, `0x408CB0`, into `+0x74`) and acquisition record (`0x40B320`),
  then `0x4648E0`; every rebuilt order node's handler state (`+0x05`, `+0x06`, `+0x0A`, `+0x4E`,
  `+0x32`, `+0x42` bits 16, 21 and 22), a builder's adoption, Paralyze's and BuildWeapon's resumed
  state, and Attack_Chase's p1 when its slot holds no weapon; the order lists of a unit the kills
  left alive and the image does not describe, torn down through `0x43A1F0`; the end countdown and the game
  end's flags bits 2 and 4 from the image; the explored map with no V bit; then `0x440A40(0,
  0x7FFF7FFF)`, controlled = viewed = P, `0x464290(V, 0)`, `0x48BD00`, and the objective's `+0x88 =
  1`;
- after the teardown: V's PlayerInfo, all `0xB9` bytes, as the Start's fill left them.

The recorder refuses a puppet level; a takeover arms it for the live level that follows, with the
fork's file handle (`tagpu_rec_fork_arm`), and it starts at the next capture point, iteration T.
`tagpu_recreader.c` reads the file and names no engine memory.

### 2.12b The replay HUD (`tagpu_hudstate.c`, `tagpu_hudinput.c`, `tagpu_vk_hud.c`; every Vulkan replay)

A replay's controls: the column of team headers and player rows with its foot plate of switches,
and the dock of transport buttons, timeline and clock, drawn at the device's resolution from
`art/hud/` while a recording plays on the Vulkan lane, and its input. Design, uploads, the input,
the check against the mockup and the gaps: [The replay HUD in game](../hud-in-game.html). **Two
byte patches, the pause icon's stub at `0x46A107` and the message lines' first row at
`0x4640D6`** (`tagpu_replay.c`, below), and no other engine detour; the second writes one word of
the engine's, DrawChatText's frame local `[esp+0x14]` (§2.5): it reads the playback's player images through the replay's calls, its stats window's
samples from the recording file through the walker's own reader, and each unit type's army value
from the level's defs on the game thread; it hit-tests clicks on the game thread against the
frame on screen, and draws a pass of its own on the render thread. **In a live recorded
skirmish** the same pass draws the live rewind's button and strip, whose clicks are the rewind's
verbs (`tagpu_hudstate_mode`, *Live rewind* in [the HUD in game](../hud-in-game.html)). On GDI a
replay has no on-screen controls and a live game no rewind button, and the engine's `PAUSED`
icon stays. The watching dock and
the player cards it replaced are deleted, and with them the dock's and the cards' HUD-scale
regions (§2.15).

| site | what we do there | thread |
|---|---|---|
| `fill_frame` (`tagpu_packet_pub.c`), after the UI's render half | `tagpu_hudstate_fill`: one `TAGPU_PK_HUD` record into the packet's area `hud_off`/`hud_len` — the clock, the speed and pause, the HUD's scale, up to 10 seats from `tagpu_replay_players`, up to 320 markers from `tagpu_hudmarks_get`, and the HUD's own state from `tagpu_hudinput_fill` (the pointer, the folds, the switches, the scroll, Sight's and Follow's seats, the key held, the toast, and the stats window's open, split, measure and place); in it, the timeline drag's seek and the window drag's move once a frame. Then, while the window is open, `tagpu_hudstate_fill_stats`: one `TAGPU_PK_HUDSTAT` into `hstat_off`/`hstat_len` — the chosen measure's samples for the record's seats from `tagpu_hudmarks_stats`, with their period and base. No area unless the Vulkan lane draws the HUD and a recording plays, or, the live HUD (`mode` `TAGPU_PK_HUD_LIVE`), a live recorded skirmish whose rewind is open or could open: then the record carries the clock, the pause, the scale, the rewind's state, span and `rw_G`, and Play from here's arm (`rw_arm`, `rw_R`, `rw_evict`), and no seats, markers or stats. A record or a stats area that does not fit sets `TAGPU_PK_TRUNC_HUD` (`0x800000`) | game |
| `frame_valid` (`tagpu_packet.c`) | refuses a HUD area whose length is not `sizeof(TAGPU_PK_HUD)` or lies outside the slot, and a record whose `nseat` or `nmark` passes its array; a stats area with no HUD record, of any length but `sizeof(TAGPU_PK_HUDSTAT)`, outside the slot, or with `npt` above 512 | render |
| `tagpu_overlay_draw`, after `tagpu_gui_present` | `tagpu_hudstate_present`: the record as the layout's state — HUD units from the viewport, the engine's frame from `tagpu_hud_live`, colours through the presented palette, the teams, the names as Latin-1, the pointer in units, Sight and Follow as rows, the stats window's groups (a player each, or a team each, summed, when the split asks for teams and they are told apart, `tagpu_hudlay_headed`; a non-finite sample as 0) — stamped with the frame's `frame_counter`, which `tagpu_hudstate_frame` compares with the pass's `frame`; and the state dump on `tagpu_replayhud.dump`, with the hit map of the frame on screen | render |
| `vk_present` (`tagpu_vk.c`) | `tagpu_vk_hud_prepare` just before `tagpu_vk_gui_prepare`, which builds two layers into one instance range after one `tagpu_huddraw_begin` (one glyph-atlas generation for both). **The record order**: the HUD's base layer (`tagpu_vk_hud_record`) after the world's composite, so the engine's UI and the cursor draw over it; the UI composite (`tagpu_vk_gui_record`); the HUD's top layer (`tagpu_vk_hud_record_top`, the ops flagged `HOPF_TOP`: the live rewind's button in the top bar, its strip and its toast); then, while the composite drawn holds the Config panel (its hand-over's `panelL..panelB`, taken at that flip), the whole composite again inside the panel's part of the top layer's rect (`tagpu_vk_gui_panel_rect`, `tagpu_vk_gui_record_whole`), so the panel stays over the strip; then, inside the top layer's rect (the union of the quads its instances emit) and only when the composite drew this frame, the composite's sharp layer again (`tagpu_vk_gui_record_sharp`, a second quad with `a.z = 1` that `LAY_FS` cuts to the sharp layer), so the cursor stays the last thing drawn there, in record order within the one render pass. It counts in the census's drawn passes (`hud=N` on the `vk: census` line) and claims no A/B frame, so an A/B capture of the swapchain image with it drawn stands down; a world capture reads the world target, which neither layer draws into, so the refusal's count leaves it out there (`if (abworld) ndraw -= draw_hud`), and a world A/B runs in a live recorded skirmish, which always draws the rewind button; the UI pass still counts, so under the play defaults a world A/B needs `tagpu_gui.off`, as before. In `prepare`, the chart's RGBA32F image (`tagpu_huddraw_chart`, 157 KB) goes whole through the slot's staging in the frame's own command buffer when its generation moves; the first frame after the build clears the image, so the HUD never waits on that upload's room, and a frame whose image is an older generation hides the chart's instances. **The groups' layer** (a control drawn whole and faded once, `tagpu_huddraw.c` *THE GROUPS*): a frame with a group records, in `prepare` and so before the seam's render pass begins, a render pass of the HUD's own (`s_lrp`) into an RGBA8 image a frame slot (sRGB when the attachment is, UNORM otherwise: the group's coverage lives in its alpha), drawn by a second pipeline built against that pass (`s_lpipe`); both layers' groups are packed into the one image, and `record` and `record_top` sample it through the composites that place each group. The image grows in steps of 64 pixels, never past the attachment, and is replaced only behind the slot's fence wait, so nothing in flight names the image it replaces (`layer_size`); one that cannot be made stops the pass (`ST_REFUSED`). A frame it will draw hands its ops to `tagpu_hudstate_drawn`, the hit map; any other frame empties the map | render |
| `tagpu_zoom_frame_end` | `tagpu_hudstate_cmd`: the hit map into the command record's `TAGPU_CMD_HUD`, with the window drag's frame (`u`, the client pixels of a HUD unit, and `W`, `H`, the screen in units), emptied as it is copied, so each drawn frame's map is posted once and a frame `vk_present` did not prepare posts none; `pass` is 1 when the HUD's prepare ran since the last record (`tagpu_hudstate_drawn` sets it, NULL first, on every frame the pass runs) and 0 otherwise, so the game thread tells "drew nothing" from "no frame was prepared" | render |
| the command record's take (`tagpu_packet_pub.c`), before every in-play draw | `tagpu_hudinput_take`: the map copied for the game thread's hit tests | game |
| `tagpu_shield_wndproc`, before the shield's gate | `tagpu_hudinput_button` at `lParam`'s client point, for both buttons' presses, double clicks and releases, after the sprocket's `tagpu_menu_click`; `tagpu_hudinput_cancel` on `WM_KILLFOCUS`, `WM_CAPTURECHANGED` and a deactivating `WM_ACTIVATEAPP` | game |
| `deliver_mouse` (`tagpu_shield.c`) | `tagpu_hudinput_button` at a device injection's client point; an injection in the engine's space passes under the HUD | game |
| `to_game` (`tagpu_shield.c`), `fake_WndProc` (`wndproc.c`) | `tagpu_hudinput_wheel`, before the zoom's, at `mouse_last_client` | game |
| `0x46A107` in `DrawGameScreen 0x468CF0`, the pause icon's test (`pause_icon_init`, `tagpu_replay.c`) | a stub over the 9-byte `test`/`je`, byte-checked with the `mov eax,[main]` before it: the pause bit tested as the engine does, and `igpaused` drawn (`0x46A110`) only when `tagpu_hudinput_on_screen` answers 0 (no control on the screen carries the pause: the record last taken has no controls, or neither a playback nor a live rewind owning the level, or the HUD is off; a paused live recorded skirmish with no rewind open keeps the icon), else `0x46A140`. Writes nothing ([engine map](../exe-reverse-engineering.html), *The replay HUD's stats window and the pause icon*) | game |
| `0x4640D6` in `DrawChatText 0x464060`, the first message line's top (`chat_top_init`, `tagpu_replay.c`) | a stub over the 8-byte `mov dword [esp+0x14],0x34`, byte-checked with DrawGameScreen's call `0x469FCB`: for that call (the return address `[esp+0x34]` past the stub's push is `0x469FD0`) it stores `*tagpu_hudinput_chat_top()` instead, for the two other callers `0x41F7A5` and `0x41F9D8` the engine's `0x34`, keeping `eax` (main, read at `0x4640DE`), and goes on at `0x4640DE`. The value is `0x34` but while the last frame drawn had the live HUD's strip, when it is the first engine row clear of the strip, so the lines start under it ([the HUD in game](../hud-in-game.html), *The top layer*). Game thread, inside the draw; it writes DrawChatText's frame local `[esp+0x14]` and nothing else of the engine's (§2.5) | game |
| `DrawGameScreen 0x468CF0` (the menu's tick) | Leave's question: the guard over `0x460680..0x4607F1` and `0x4605C0..0x46065C`, `0x512FF8 = 0`, the opener `0x460680`, the title through `0x4A07D0`; no `ActDone`, outside the pump; every frame, the pause Leave held put back once `YESORNO.GUI` is off the stack (`tagpu_hudinput_leave_shown`) | game |
| the marker walker (`tagpu_hudmarks.c`) | reads the recording file through its own `RecReader` and publishes markers and the stats window's samples; started by the first fill that finds the playback live, after the game thread has copied each unit type's army value out of the level's defs (`tagpu_replay_army_values`: `main+0x1439B`, count `main+0x1438F`, def `+0x18A`, `+0x22F`, `+0x245`) into a table it frees after the join | its own; the table's copy, game |
| teardown wrap's entry (`tagpu_reclaim.c`) | after `tagpu_menu_level_end`: `tagpu_hudmarks_level_end` (the walker stopped and joined, the markers and bookmarks forgotten) and `tagpu_hudinput_level_end` (the map, the held press, the folds, the scroll, the toast, the stats window's open and its drag, Leave's question), before `tagpu_replay_level_end` | game |
| `vk_down`, `passes_down` | `tagpu_vk_hud_down`, behind the seam's `vkDeviceWaitIdle`; the pass builds again on its next frame | render |

**Fields we write**, through the replay's calls and `tagpu_menu.c`, on the game thread, on a
playback level only: `main+0x38A51` bit 0 (the pause, read-modify-written); `main+0x38A4B` and
`+0x38A4D` (the speed's ceiling and live value, the presets 5..80 only, the player's put back at
the teardown and exchanged around every settings save); `main+0x2A43` and the temporary sight
records for Sight, and the camera for Jump and Follow, as [the replay's players and
Sight](../tadr-port/demo-recorder-player-cards.html) and the perspective plan's 4c-2 state; the dword
`0x512FF8` (YESORNO's question, 0) and, through `0x4A07D0`, the `TITLE` record's text of the
YESORNO it opened and `gi+0xCCA`. Of our own: `replays\index.dat`'s bookmarks field, the store's
`replayhud` (`impure.cfg`), the game folder's `tagpu_replayhud.json` (and its `.tmp`), and the
deletion of `tagpu_replayhud.dump`. Nothing the recording carries. The live HUD writes no field
of its own: its clicks are the live rewind's verbs, queued through `tagpu_replay_rewind`, which
write what the `rewind` trigger's do ([live rewind](../tadr-port/demo-recorder-s4-rewind.html)).

**Fields we read** beyond the replay's calls: `main+0x38A51` bit 0, in the pause icon's stub;
the level's unit defs as listed above, on the game thread; and, in the walker, nothing of the
engine's — the recording's player images (`+0x90`, `+0x9C`, `+0xD4`, `+0xFC`, `+0xFE`, `+0x144`,
with `+0x106` for the markers) and unit images (`+0xA6`, `+0xFF`, `+0x104`, `+0x110` bit 28).
