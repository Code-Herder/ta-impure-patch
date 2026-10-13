# Demo recorder — the skirmish slice (the plan)

## Status

The recorder writes recording format 19 and its readers take format 19 alone (*The file*, below).
Enabled native multiplayer recording carries an observed world, a contribution container and cut
footer, plus replica creation roots. Native engine playback remains unfinished; the container
and its verification are in the [multiplayer slice](demo-recorder-multiplayer-slice.md#contribution-file-container).

The [product page](demo-recorder.md#milestones-and-delivery-order)'s second milestone, planned
2026-09-29: recording, puppet watching, seeking, live rewind and takeover, forks, the Replays browser
and `tacli` automation, with the verification the
[completeness design](demo-recorder-completeness.md#build-order) lists as its step 2. Six landings,
each a finished unit verified by running it. **S1, the recorder, landed on 2026-09-29**: a
skirmish is recorded to a file when `tagpu_recorder.on` asks for it, and `tools/replay/tarec.py`
reads a recording back. **S2, watching, is two landings.** S2a landed on 2026-09-30: a recording
carries every mover, order list and PlayerInfo, and the events, each checked against the engine
(*S2a, verified and measured*). S2b landed on 2026-09-30: `tacli replay play` plays a recording
back on puppets, showing everything or one recorded player's view, and every recorded value is
equal to the recording at every checkpoint (*S2b, verified and measured*). **S3, seeking, is
three landings** (*S3 — seeking*). S3a landed on 2026-09-30: a recording carries the feature grid
and the wreck pool, and every change to them is replayed through the function that made it
(*S3a, verified and measured*). S3b (2026-10-01) records the main block's and
`.data`'s simulation fields, the projectiles in flight and the explored map, and moves a playback
to any GameTime of its recording, forward or backward, from a fresh process or in the running one
(`tacli replay seek`; *S3b, verified and measured*).
**S3c, the round trip and the harness, is three landings** (*S3c*). S3c-1 (2026-10-01) makes the
recording the only writer of every declared table on a puppet host, adds frame dumps and the
puppet round trip, and brings `tools/replay/tareplay.py` with nine fixtures in the tree
(*S3c-1, verified and measured*). S3c-2 (2026-10-01) counts the engine's roots and sinks on the
recording machine and on puppets and compares them, gates the two images on a null twin, scans
dumps for addresses recorded as values, and makes each feature root at its own phase of the step
(*S3c-2, verified and measured*). S3c-3 (2026-10-01) declares every simulation region with the
fixtures that reach it, adds the coverage build, and makes the harness a landing gate —
`tareplay.py landing`, CLAUDE.md's replay gate and the landing command's Step 4b — with `broad` as
the command run before every release; and a seek makes the viewed seat's temporary sight
records again (*S3c-3, verified and measured*). **S3 is complete. S4 provides
takeover, fork management and live rewind through tacli** below.

## How the slice is cut

Each landing adds one capability that can be verified by itself, and the ones after it build on it.

| Landing | Delivers | Verified by |
|---|---|---|
| **S1 — the recorder** | the capture at `0x4954ED`, the writer thread, the file, self-verification, and a tool that reads a recording back | self-verification over whole games; the file read back to the tick of a dump and compared with the dump byte for byte; the cost and the size measured |
| **S2a — what watching needs, recorded** | the objects a unit record points at (the mover, the order lists) and PlayerInfo, events at their roots, the nano spray, the header's game options, seats and weapon fingerprint | S1's checks over the new records; the events checked against the images |
| **S2b — watching** | the puppet host, playback, the one-writer rule, getter 75 per seat, `tacli replay play` | a recording played on puppets and compared with the recording at checkpoints, in full visibility and from the recording viewer's perspective with its radar picture; each event kind counted as played or not |
| **S3 — seeking** | the rebuild in place from a snapshot, features and wrecks, the main block and `.data`, the replay harness and its puppet round trip, the CLAUDE.md replay gate and the landing command's Step 4b, the landing and broad tiers | the round trip, with its rules against a vacuous pass; seek times against the target |
| **S4 — rewind, takeover and forks** | the timeline in a live skirmish, Back to now, Play from here, forks in the file, the takeover from a recording | the harness's takeover run, judged like the compatibility suite's battle; the fork rules |
| **S5 — the browser and the controls** | the Replays browser, the timeline bar, perspectives, camera follow, the overlays, the recording switch in the settings screen | the browser and the controls driven through `tacli ui`, with pictures |

**S1 and S2 record every value exactly, at every tick.** That is what makes their checks exact. The
reduced stream — the same exact values, coded losslessly ([making recordings
smaller](demo-recorder-size.md)) — is a landing of its own after S2; S1 measures the exact stream's
size so that the gap to the budget is known before then. Until that landing a skirmish is recorded only
when asked, by the file `tagpu_recorder.on` in the game's folder, because the exact stream is too
large to write for every game (*Measured*). That landing turns recording on for every skirmish and
retires the file, and it packs the stream tighter as well: a scratch repacking of the exact stream
measured about 2.5 times smaller (*Measured*).

## S1 — the recorder

### What it records

At every iteration of the sim step in a skirmish, from the level's first iteration to its teardown,
the capture takes an **image** of every live unit record, of the ten player records (not the
eleventh, which nothing activates) and of the rows of every `SIM_STATE` declaration but two kinds:
the engine pools the raised limits moved into the DLL (`SIMD_ENGINE`), whose record layouts are not
written yet, and TLS slots, which are per thread. A skirmish is game mode 2, `+0` of the object
`[main+0x391E9]` points at (1 is a campaign and 3 a network game: `0x435100` is `mov eax,[ecx]`,
called with `ecx = [main+0x391E9]` at `0x41FB6C` and `0x4971C1`, DISASSEMBLED); a skirmish loaded
from a save is mode 2 too (run 3). A unit slot is live while bit 28 of its `+0x110` is set.

An image has the record's shape, and `inc/tagpu_replay_layout.h` — generated from the exclusion file
the owner controls — is all that shapes it. A byte of a value field is copied. An `id:<table>` row
holds its target's ID in place of the pointer: one more than the pointer's row in that table, by
the table's offset rule, so `NULL` is 0. A pointer outside its table and the off-map sort bucket get
IDs of their own (`REC_ID_BAD`, `REC_ID_OFFMAP`), the first counted and its first eight logged. The
bytes of `object`, `cache`, `timer` and `drop` rows are zero. A declared table's image follows its
layout the same way: an `SIMF_ADDR` field with an ID table holds the ID, and an address with none,
an `SIMF_LOCAL` value and the `SIMF_OWNED` and `SIMF_COLUMN` pointers are zero. So a record of zeros
images to zeros, and a row of zeros costs the ring nothing.

### The capture

`tagpu_recorder.c` runs first in B2's stub at the sim step's call of the unit tick, `0x4954ED`,
which then rebuilds the air pool and jumps to the tick. GameTime has just been incremented
(`0x4954C0`) and every phase of the previous iteration has run, so the capture sees the world
between two iterations, on the thread that owns it. A skirmish calls the step with 0 (`0x49693C`),
so the network pump inside it (`0x4954C8`) does not run: only a network game pumps there. It
names engine memory only to read it, each read bounded by the engine's own count
([the engine map](../exe-reverse-engineering.md#the-demo-recorders-capture-where-it-runs-and-what-it-reads-disassembled-measured-2026-09-29)).
What it writes is its own: its shadows, the ring and the harness's dump.

It images only what may have changed, and puts only the changes into the ring:

- **A unit record** whose bytes equal the copy the last capture read is passed over. Within a
  recording an image is a function of its record's bytes alone, since the tables its IDs index — the
  unit array, the unit types, the players, the weapons, the buckets — do not move until the level
  ends, and that ends the recording. A record whose bytes moved is imaged, and its image's changed
  dwords go into the ring as a delta; a new unit, and a slot whose type changed, go whole; a death
  goes as a death.
- **A player record** is imaged every iteration, and its changed dwords go as a delta.
- **A declared table's rows** whose bytes may have moved are imaged, and each row's changed runs of
  bytes go as spans, bridging unchanged gaps of up to 8 bytes. A static table's rows are found by
  comparing its bytes with the copy the last capture read. A heap table's are the rows on the pages
  the OS says were written: `SIM_ALLOC` gives every heap table `MEM_WRITE_WATCH`, and
  `GetWriteWatch` answers and resets in one call. During play only the game thread writes these
  tables, the thread the capture runs on, so no write falls between the answer and the reset. Every
  row is imaged at the first capture, when a heap table moves or `GetWriteWatch` refuses it, and
  when a table's count takes rows in; a row of zeros is passed over without imaging, its image
  being zeros, like the shadow it is compared with.

The largest declared tables are heap tables for this reason: the build ghost's memo `s_ghost`
(16,384 rows of 52 bytes) and the transported explosions' marks `s_txMark` and `s_txKillMark`
(65,536 entries each). As static arrays their 1.3 MB was compared every iteration. Each is allocated
when its module installs and never freed, and every path that can run without that module tests it
for NULL. A capture that imaged every row of every table each iteration cost 3.6 ms; write-watched
heap tables, the byte comparisons and passing rows of zeros over bring it to about 80 µs in a
four-player game.

### Self-verification

Every iteration the capture also puts fresh images into the ring, taken straight from engine memory
whatever changed: the next thirtieth of the unit slots, live or not, so every slot is checked once
every 30 iterations; one player record, in turn; and the next 16 KB of declared rows, whole rows,
table after table. After applying the iteration's changes the writer compares them with its copy.
A difference writes the fresh image into the stream as a correction, counted by record, table and
offset in the end block (256 counters, then one for the rest), and the first 16 corrections of a
recording are logged with the two values. The change stream carries whole images, so a correction
is a defect in the recorder's own pipeline: the verification runs require none, and run 5 shows the
check is not vacuous.

### The file

One file per recording, `replays\<date> <time> <map>.imprec` in the game's folder, or its first free
` (2)`…` (9)` variant, written by the writer thread alone. It is `IMPREC\r\n` and then blocks: a
32-byte header — the block's kind, its flags, the first and last GameTime it covers, its raw and
stored sizes, a CRC-32 of the stored bytes and one of the header — and the payload, packed with
the vendored zstd 1.5.7 (`src/zstd.c`) in one of three ways (`inc/tagpu_recfmt.h`, *THE RUN AND
THE CHAIN*):

- **a sectioned stream block** is a chunk of its *run*'s zstd frame. A run is the sectioned blocks
  from an `RBF_FRAME_RESET` block to the next reset, stream block of plain records or snapshot
  (blocks of other kinds between them, `RB_EXACT` and `RB_CONTRIBUTIONS`, neither end nor touch a
  run): one frame at level
  19, with a window of at most 2^20, no content size and no checksum, begun at the reset block and
  never ended, each block's bytes flushed at its end. A block decodes only after every chunk of its
  run before it, as its codec frames do; it is never stored.
- **a snapshot** is a *key*, one zstd frame of its own at level 19 or the bytes stored, or
  `RBF_CHAINED`, one frame whose raw-content prefix is the records of the segment's snapshot before
  it. The segment's first snapshot and every tenth after it are keys (`REC_CHAIN_MAX` 9 links), as
  is any whose chained frame fails the writer's check; a writer without the codec writes every
  snapshot as a key at level 3.
- **every other block** is exactly one zstd frame of the raw bytes at level 3 when that is smaller,
  else the bytes stored. The header, the end and the timelines blocks are always stored, so a
  header reads without zstd.

`inc/tagpu_recfmt.h` is the format, `REC_FORMAT` 19; a reader takes that format alone:

- **the header block**: the format version, Impure's build, the game mode, the map, the first
  GameTime, the wall clock, the unit array's slots and the units each player may have, the unit
  types' count and a hash of their names, the players (index, controller, side, logo and name), the
  registry's levers and their text, the exclusion file's SHA-256, and the recorded declarations
  (index, class, key, row size, flags and name), so a reader can walk their images; the main
  menu's version text, the mounted archives and the mod's fingerprint (*What the Replays browser
  asks of a recording*); the unit frame's constants (below): `RT_CODEC`, `RT_PHYSICS`, `RT_AIR`;
  and in a skirmish recording `RT_HOLD`: the refresh interval (300 ticks), the hold table's
  masks and its SHA-256, what the stream may hold between two refreshes, and `RECL_SPRAYS`, its
  nano sprays written as spray sessions (the lossy filter,
  [making recordings smaller](demo-recorder-size.md#l5-the-lossy-plan-measured-offline));
- **a minimap block** right after it, when the map has a picture;
- **stream blocks** of whole iterations, written by the lossy filter from the writer's exact
  records, each ending at the first iteration past 2 MiB, at 30 iterations, or at a refresh — the
  segment's first two iterations and then every GameTime 1 modulo the interval (the first
  iteration past one a gap skips; `reclossy_is_refresh`), where the stream's world
  equals the writer's copy, checked — which flags it `RBF_REFRESH`; under the test lever
  `tagpu_rec.exact` each is followed by an `RB_EXACT` block of its exact records, which a reader
  of the stream skips. A skirmish stream leaves out each nano spray its unit's spray session
  (`RK_SPRAY_SESSION`, written after the iteration's last record) predicts: the unit's spray of 1,
  2 or 4 GameTimes before, its point moved with the unit's position change when the session
  follows it; every reader makes it again after the iteration's events from a table of each
  unit's sprays of the last four GameTimes, which a snapshot's iteration empties (it writes every
  spray and closes every session) and every reader empties there too;
- **a snapshot block** at the first refresh 1,800 iterations or more after the last
  (`SNAPSHOT_TICKS`, 60 seconds at normal speed), built from the writer's copy, with only the cells that differ from the grid at the end of the
  segment's first iteration (the base), only the wreck records that differ from the pool then (the
  wreck base; a record now zero included), both bases taken there by the writer's
  `iteration_done` and the reader's `tick_take`, and no table span that is all zero. The copy's
  grid takes exactly the cells every reader takes as images: the writer refuses cells after an
  event of their iteration outside the feature tick's call, and `RK_GRID` anywhere but before the
  first iteration's events (`feature_record`), so a seeking reader's grid is the stream's by
  construction. The level load links every free wreck
  record to its neighbours, so none is zero and a snapshot that stored every record that is not
  zero stored the whole pool; against the base the battle's 8 snapshots carry 5,524 wreck records
  instead of 65,536. A snapshot that carries more records than differ reads the same; a dump
  carries every record that is not zero, read over a zero pool. The interval is the owner's
  choice at the size plan's L4 (2026-10-10): a seek reads at most one interval from a snapshot,
  261–265 ms at worst on a light four-player hour and 538–550 ms on a heavy one, and about 1.7 s
  of decode in the big battle's opening, so the one-second target is relaxed for dense battles
  ([making recordings smaller](demo-recorder-size.md#l4-the-measured-hours-the-snapshot-interval-and-r1));
- **the end block**: why the recording ended, the last GameTime, the iterations, the corrections,
  the id rows found outside their tables, the most the ring held, the game's end flags, each
  seat's result, first commander death and elimination, and the stream blocks the codec
  declined (`RT_CODEC_DECLINED`) and whose check failed (`RT_CODEC_MISMATCH`).

A block is written whole or not at all, so a file cut short reads up to its last complete block, and
a file with no end block is incomplete.

**The sectioned container.** A stream block with `RBF_SECTIONS` holds, in place of its records, a
container that decodes to exactly those records, so everything that reads a block's payload — the
C reader's `apply`, `tarec.py`, `recsize.py` — sees the records as before. It is a 40-byte
`RecSecHead` (the payload's bytes, the iterations, the section count and each section's length),
then eight sections, in this order: the record kinds in payload order, each iteration's as a
*pattern* of runs (`RCS_KINDS`, below); the
**unit frame**, which makes every `RK_UNIT_DELTA` (`RCS_UNITS`); the **pool frame**, which makes
every `RK_PROJ_HEAD`, `RK_PROJ` and `RK_PROJ_DELTA` (`RCS_PROJ`); the **cells frame**
(`RCS_CELLS`, every `RK_CELLS`), the **orders frame** (`RCS_ORDERS`, every `RK_ORDERS`) and the
**events frame** (`RCS_EVENTS`, `RK_EV_CREATE..RK_EV_DEATH_END`, `RK_EV_NET_CREATE`, `RK_EV_SLICE`,
`RK_EV_QUERY` and `RK_EV_YARD`); the bodies of every other kind verbatim, each behind a varint length (`RCS_OTHER`: the block's first
tick -- iteration i's GameTime is the first's + i, and a block whose GameTimes are not consecutive
is written as records -- the unit images and gones, the tables, the perspective kinds and the
rest); and the **state frame**
(`RCS_STATE`, every `RK_GLOBALS`, `RK_GLOBALS_DELTA`, `RK_PLAYER`, `RK_PLAYER_DELTA`, `RK_WRECK`
and `RK_FEATURES`). A kind added later falls into `RCS_OTHER` on both sides (`rec_section_of`),
with no table to update.

**The kinds** (`tagpu_recfmt.h`, *RCS_KINDS*). Each iteration's pattern is a reference into the
run's last 16 patterns (a byte, 1..16, the entry moving to the front) or a new one (0, its length,
its bytes). A pattern is the iteration's runs, a varint kind and a varint count each, with two
markers that stand for records no frame codes -- 0 an empty feature tick, its root and end the
bodies of the run's last explicit one, and 0xFFFF an `RK_FEATURES` that is the state frame's
prediction -- and no count on the first run of three groups, whose totals the frames give: the
iteration's `RK_TICK` (1), its `RK_UNIT_DELTA` (the unit frame's changed units) and its `RK_PROJ`
and `RK_PROJ_DELTA` together (the pool frame's changed records). A reader fills a group's count
once its frame has made the iteration and refuses a group whose other runs reach its total.

**The unit frame** (`src/tagpu_reccodec.c`) keeps, per slot, the image at the end of the previous
iteration, the one before it, five lanes of the one before that and which of four candidates last
predicted each lane: the engine's own rule, the previous value, the linear extrapolation, or for an
aircraft's horizontal velocity the steering term turned by its last change. The image is cut into
lanes by the header's `RT_CODEC`, the rows of `tagpu/ddraw/replay-lanes.tsv` (112 lanes;
`tools/replay-lanes-gen.py` checks the file and generates `inc/tagpu_reclanes.h`), and the rules
find their lanes by role, never by offset; since each recording carries its own table, editing the
TSV never orphans a file, and a wrong width costs size only. A unit is listed in an iteration only
when a lane differs from its prediction, and then as residuals per lane. A unit *at rest* — its
image equal to the two before it and to the third's lanes, and either not moving or with a move
stamp older than the step before, and with its ledger's requested and granted zero — is skipped
without evaluating it, which is exact because it is a
fixed point of every rule (the argument beside `slot_advance`). The rules — heading by turn rate, ground
velocity from the sine table, position by velocity, the footprint and sight cells, the claim and
move stamps, the countdowns, the resource ledger's requested and granted, and for aircraft the air
step's velocity, speed, smoothed acceleration and bank — and the engine facts behind each are in the engine map's
[*The demo recorder's unit frame*](../exe-reverse-engineering.md#the-demo-recorders-unit-frame-the-engine-rules-it-predicts-from-disassembled-measured-2026-10-08).
They are integer arithmetic only (the ledger's float sum included, `reccodec_f32_add`), so the writer thread's x87 precision does not matter and the
Python reader (`tools/replay/reccodec.py`, an independent implementation in numpy and numba)
agrees with the C one byte for byte. The capture writes the constants the rules need into the
header at the recording's start, on the game thread: `RT_PHYSICS` (gravity `main+0x14263`, sea level
`main+0x1427F`, LosType bit 2 at the segment's start) and one `RT_AIR` row per aircraft type (def
`+0x241` bit 11; its sight height `+0x170` and the five flight fields), at most 1,024 rows; a type
without one is predicted as a ground unit, which costs size only.

**The pool frame** (`src/tagpu_reccodec.c`, THE POOL FRAME) codes the projectile pool by identity.
The engine's compaction keeps the survivors in order at the front of the pool and appends at the
count, so each iteration is written as its new count and extent, the old records that died (as
gaps), the births (the count's growth) and the stale slots past the count that changed. A survivor
is predicted dword by dword from its own last image and the one before it: position by velocity
with the last step's drift, a guided weapon's velocity from its heading, pitch and speed (when its
last image already follows that rule), the `+0x67` stamp as its old slot, otherwise the previous
value or the linear extrapolation, whichever was closest last. A birth or a changed stale slot is
a reference — the zero row, or a row of the new pool so far, of the old pool, of the old pool
advanced a step, or one made from the last iteration's `RK_EV_SHOT` events and the last birth of
that weapon — and residuals against it; a birth also takes the stamp of the slot it was appended
at. The decoder rebuilds the pool and makes each projectile record from it: an iteration's records
are exactly its changed slots below the extent, ascending, and at most one `RK_PROJ_HEAD`, which it
carries whenever the count or the extent changes. The frame's one parameter is the header's `RT_PROJECTILES`; its constants
— the dword order, the four sections and their anchors, the stamp, the guided rule and the sine
table — are the format's. The engine facts are the engine map's
[*The demo recorder's pool frame*](../exe-reverse-engineering.md#the-demo-recorders-pool-frame-the-engine-facts-it-predicts-from-disassembled-measured-2026-10-08).

**The record frames** (`inc/tagpu_recframe.h`, and one file each, `src/tagpu_recframe_cells.c`,
`_orders.c`, `_events.c` and `_state.c`, whose opening comments hold THE BYTES, THE STATE and THE
BOUNDS) make the bodies of their kinds, in payload order, from a section of columns: a `u8` scheme,
a varint column count (1..32), each column's varint length, then the columns, which fill the
section. A reader takes only its own scheme (2 in the cells frame, 1 in the others) and column count, and reads every
column to its end, so a file of another build's coding is refused, never misread. The encoder walks
each iteration's records in order after the unit and pool frames (`frames_walk`), giving each frame
record to its frame and applying `RK_UNIT` and `RK_UNIT_GONE` to the unit copy as they come, so a
frame sees at each record the same unit copy the decoder's walk has there (`RecFrameAt`). What each
frame codes against:

- **cells**, 9 columns: a copy of the grid, restarted at every reset point from the grid the
  reader holds there, and the last iteration's tick input. An `RK_CELLS` is an image when no event came earlier in its iteration, else tick input
  (the reader's rule); an iteration's images that repeat the last tick input record for record are
  one flag byte (the recorder's `cells_tick` leaves its shadow as it was, so they usually do); other
  records are runs as position gaps, and each cell as the six fields that differ from the copy. The
  tick input is kept up to `CELLS_HELD`, 16,384 cells; past it the next iteration has none to repeat.
- **orders**, 26 columns: the last whole image of each (slot, list), in an arena of at most 65,536
  nodes. A list re-sent at the length it had is a 22-bit mask a node of the dwords that changed, and
  those dwords; any other list is its nodes raw. A list that does not fit the arena is not kept,
  decided from the records alone.
- **events**, 31 columns, one a field: IDs against the last ID; a shot's start and target point
  against its units' positions in the unit copy; a start's arguments against the slot's last
  arguments for that script; script names and reply texts by a dictionary of the first 256 since the
  reset; a spray as the slot's last again, its reverse and tail with point deltas, or raw; a death's
  image as a mask of the dwords that differ from the unit copy and those dwords.
- **state**, 19 columns: each changed global as a delta of deltas, or as a count of Park–Miller
  steps from its last value when the step reaches it within the search (1..16,384 steps, at most two
  dwords an iteration: it is how the simulation RNG's seed moves), or raw; a player's changed dwords
  as deltas of deltas; a wreck record as a mask of the u16s that changed; the feature heads as last
  time and the cursor by the feature tick's own rule.

Each frame's state — and every latch — is cleared at every reset point, except the cells frame's
copy of the grid, which restarts from the reader's grid there; so a frame's state is a function of
the records since the reset, of that grid and of the unit copy alone. A frame never declines mid-block: one record it
cannot carry back byte for byte (a pad that is not zero, a length not its kind's, a value past its
bounds) or a failed allocation latches inside it, and its section's write then declines the block
after its iterations. The engine facts the codings rest on are the engine map's
[*The demo recorder's record frames*](../exe-reverse-engineering.md#the-demo-recorders-record-frames-the-engine-facts-they-predict-from-disassembled-measured-2026-10-10);
each is a predictor's, so a wrong one costs size, never data.

**Reset points.** The frames' state crosses block boundaries only inside a run of sectioned
blocks. All restart — `RBF_FRAME_RESET` — at a segment's first stream block, at the first after a
snapshot and at the first after a stream block of records: the unit and pool frames from the units
and the projectile pool the reader holds at the end of the iteration before it, the cells frame from
the grid it holds there, and the orders, events and state frames empty. That grid is the same bytes
in the writer's encoder, its check decoder and every reader by construction: each builds it from
zero at the segment's start by one rule, an `RK_CELLS` being an image when no `RCS_EVENTS` record
came before it in its iteration (the encoder keeps it through declined blocks too, `plain_apply`),
and a seeking reader's comes from a snapshot built by the same rule (`tagpu_reccodec.c`,
`reccodec_stream_block`'s INVARIANT). A restart from a grid of another size, or none, leaves the
codec invalid: the reader refuses the block, the writer declines it. So a block inside a run decodes only after its predecessors since
the run's reset: a reader starts at a snapshot or a segment's start (the seek already does), and a
sectioned block met with no frame state is refused by name, never misread. The run's zstd frame
restarts with the codec's frames and follows the same rule: the readers hold one validity for both,
and any block they refuse leaves both to be restarted (*The file*).

**Decode before write, and the fallback.** The writer encodes each stream block, packs it as its
run's next chunk, unpacks that chunk with a decoder that has unpacked the run's chunks before it, as
a reader's has, and decodes the container with a second codec state, started from the units alone
as a reader's is, and writes the sectioned block only when that gives back the container and then
the records byte for byte. Otherwise
it writes the block as records — **declined** when the encoder met what the frame does not carry
(a delta on a slot that is not live or that an `RK_UNIT` or `RK_UNIT_GONE` names in the same
iteration, deltas whose slots do not ascend, a bitmap that is not exactly the dwords that differ
from the copy; projectile records that are not exactly the changed slots below the extent,
ascending, each once, after at most one head that does not lower the extent — a writer's
correction names a slot twice; a record a record frame cannot carry back byte for byte; a block of
more than 256 iterations, a frame past 16 MiB, no memory; zstd without memory for the run, which
the writer logs apart as "no memory"),
**MISMATCHED** when the check differed, which is a codec bug — and the next block restarts the
frames and the run. A codec bug therefore costs size, never data. The end
block counts both; the writer logs the first four of each with their GameTime range, and its
closing line gives `stream N KB of records written as M KB; K blocks: S sectioned, D declined, X
MISMATCHED; the codec cost T ms, at most U ms for one block; the ring held R KB at most; N
snapshots, K of them keys, packed in T ms, at most U ms for one`. `tareplay.py` fails a fixture
whose recording's end block names any mismatch ("the stream codec did not decode back in N
blocks") or which has no sectioned stream block at all ("the codec never ran": a writer without
memory for the codec counts 0 declined and 0 mismatched), and reports the declined count. Without `RT_CODEC`
and `RT_PHYSICS` (a dump, a synthetic test file), with a grid of more than `RECC_GRID_MAX`
(4,194,304) cells or a side past 65,535, with a wreck pool of more than 65,536 records (the bounds
`reccodec_params` sizes the record frames by, from `RT_FEATURES`), or without memory for the
encoder's and check decoder's states, every stream block is written as records; without memory for the header's codec
constants the recording ends before its file is opened.

**The decline on demand.** No fixture's recording declines a block of itself (a real game still
can, for one: a frame's section past 16 MiB declines its block), so the fallback and the restart
after it would run in no fixture. The test lever `tagpu_rec.decline` — read at attach, honoured in a test launch
only (`tagpu_rec_lever_init`) — makes the writer's codec decline stream blocks 2, 7, 12, …:
`reccodec_stream_block` invalidates the encoder before `reccodec_encode`, whose first branch then
runs (`plain_apply` of the whole block, the encoder left invalid, `RECC_DECLINED`, the branch a
block the prepass refuses takes too), and then the records block and `RBF_FRAME_RESET` on the next.
That is one of the encoder's three decline paths.
**No fixture reaches the other two**: the decline in mid-block, where `enc_iteration` refuses an
iteration and `plain_apply` takes the records from it on, and the declines after the walk (the unit
frame's `over`, a column's failed allocation, `units_write`, `pool_write`, `frames_write`), which
return with nothing left to apply. The recording's header names the lever like every lever, and
names it whenever the file is present (`tagpu_sim_lever` notes it before the test-launch gate
refuses it, as it does `tagpu_hold.on`): the header says the file was there, not that blocks were
declined; the end block's declined count says whether any were. The fixtures `codec-decline` (the `forest-fire`
fixture's scenario, `s3a-forest-fire.json`, ended at GameTime 2400 instead of 4500) and its
stock-limits twin `stock-codec-decline` (`replay-stock-decline.json`, ended at 2400), both with
`"decline": true`, record under it, play, round-trip and seek across the declined blocks like any
fixture, and their stream codec check also fails when the recording declined none. Two, because the
landing tier asks each build that compiles a changed function to reach it. The stock twin plays a
scenario where no weapon fires -- `resource-snapping.json`'s setup, the human seat's builders and
one idle combat unit, the AI seat a lone solar collector across the map, with the advanced
constructor patrolling to the map's corner and back all level so that the declined blocks carry
images and the stock build reaches the frames' plain path -- because a seek on the stock-limits build leaves the engine's
own explosion, debris and flying-piece pools empty (the owner-approved known gap in
`demo-recorder-completeness.md`, scoped to `stock-secondary-orders`): a battle there fails the
strict images gate whenever its T1 finds an explosion alive.

**What a reader refuses**, the C reader and `reccodec.py` alike, each by name (the C reader with the
block's file offset): a flag bit it does not know, `RBF_SECTIONS` or `RBF_REFRESH` on a block that
is not a stream block,
`RBF_FRAME_RESET` without it, a stored sectioned block, `RBF_CHAINED` on a stored block or on any
block but a snapshot, a packed header, end or timelines block, a block whose header passed its CRC
but whose raw or stored size passes 64 MiB (`RECR_BLOCK_MAX`), a packed body of no bytes, a body
that is not exactly one zstd frame of its `raw` bytes with nothing after it or whose frame asks for
a window past 2^27 (zstd's streaming default, bounded by name on both sides since the C one-shot
decompression does not bound it); a sectioned block's chunk read anywhere but at its run's reset
block or right after its run's previous block, a run whose first chunk is not a zstd frame header
asking for a window of at most 2^20 with no content size, no dictionary and no checksum, and a
chunk that does not give exactly its `raw` bytes from all of its input or that ends its frame; a
chained snapshot with no snapshot before it in its segment, more than nine links from its key, or
whose frame does not give its `raw` bytes against that prefix; a malformed or
repeated `RT_CODEC`, `RT_PHYSICS` or `RT_AIR` (lanes that do not tile the 0x148-byte image, a role
missing, twice or of the wrong width, air types not ascending); a sectioned block in a segment
whose header lacks `RT_CODEC` and `RT_PHYSICS`; a segment whose unit slots differ from the first
header's; a container whose head, kinds, lengths, slots, masks or columns exceed their bounds, which
leaves bytes in a section, or which decodes to another payload length; a pool frame whose
iterations are not the head's, whose count or extent leaves `[0, RT_PROJECTILES]` or whose count
passes its extent, which names more dead slots than the old count or a dead or stale slot out of
its range, more survivors than the new count, a reference past its section, fewer bytes than its
codes and columns need, or bytes after them, or whose iteration's projectile records are not its
changed slots or carry no head (or two) where the count or the extent changed; a record frame's
section with another scheme or column count, a column table cut short, a column past its section,
bytes after the columns or left in a column, or a value past that frame's bounds (each file's THE
BOUNDS: a run off the grid, a slot past the array, a dictionary index past the entries held, a
step count past 16,384, a wreck index past the pool, a flag that is not 0 or 1, …), and a body a frame would make past 65,535 bytes; and an unflagged sectioned block with
no frame state. Every operand of a rule is clamped before it is multiplied, so no value
in a file can overflow the arithmetic.

**Memory.** A codec state is 804 bytes a slot (the copy and L2, `0x148` bytes each, L3's 16, the
winners' 128 and four flags, as `reccodec_new` allocates them) and 613 bytes a projectile record
(the pool and the image it is making, both one iteration before, the winners, the ages, the
advanced image and the survivors' maps), plus a 128 KB air index, 1,425,528 bytes of the pool
frame's tables (the last birth of each of 4,096 weapon IDs, 4,096 shot events and their rows,
and the encoder's search over at least 4,096 rows) and the scratch of the largest block it has
met: before their scratch, 14,832,808 bytes, about 14.8 MB, at 5,001 slots and 3,000 records;
30,912,808, about 30.9 MB, at 15,001. The record frames add, by their files' counts, 13 bytes a
grid cell (the copy: 5.4 MB on a 642 × 648 grid) and 0.6 MB of tick input (cells); 12 bytes a (slot, list) and an arena that doubles up to 65,536 nodes, 5.8 MB (orders);
128 bytes a slot and 40 KB of dictionaries (events, 0.68 MB at 5,001 slots); and about 15 KB and
49 bytes a wreck record (state, 0.4 MB at 8,192): up to about 13 MB a codec on that grid, and
about 62.5 MB at `RECC_GRID_MAX`, 54.5 MB of it the grid's copy. The writer holds two codecs, a
reader one, made at its first sectioned block. A reset copies the grid whole into the cells frame,
once a reset in a sequential reader and in the check decoder and once a seek.

### The game never waits

The ring is a single-producer, single-consumer byte ring of 16 MB, allocated when the recording
starts. The capture writes an iteration's records and then publishes its write count with an
interlocked exchange; the writer reads the count before the records and gives bytes back the same
way. 32 bytes are always kept back, so the capture can end a recording whatever the ring holds. If
an iteration's records do not fit, the capture drops them unpublished and ends the recording after
the last whole iteration, marked incomplete; a write error ends it the same way, through a flag the
writer sets and the capture reads.

The capture never waits for the writer. It signals the writer's event at the start, whenever a
megabyte has been published since its last signal, and at the end; the writer also wakes every
100 ms. A signal under Wine is a round trip to the wineserver, which a busy server can hold for
milliseconds, and signalling every iteration made the worst iteration 2 to 4 ms. What the writer may
read, and when it may free the ring, rest on the counts alone (`tagpu_recorder.h`, *The hand-over*).

Nothing on the game thread allocates once a recording has started but one thing: the extra weapons'
side table is allocated at the level's first unit with more than three weapons (`tagpu_weapons.c`),
and the capture then grows that table's shadow, once. If it cannot, the recording ends, marked
incomplete.

### Its lifetime

A recording starts at the first capture of a skirmish level, and only when it is asked for and
its end is certain. `tagpu_recorder.on` in the game's folder asks for it: the recorder looks for the
file once, at that capture, so it has to be there before the level starts, and removing it changes
the next level, not this one. The end is certain because `tagpu_reclaim`'s wrap of the teardown
`0x491B60` tells the recorder on the game thread, on entry, before the cascade frees the unit array
(`0x485980`, called at `0x491B95`), and without the wrap the recorder does not start. The end
record is the capture's last access to the ring; the writer then
finishes the file, frees the ring and exits. Each recording owns its ring and its writer, so a level
that starts while the previous file is still being finished takes a new pair.

A recording that has ended is complete once its writer has closed the file, and **the process's
exit waits for that**: the in-game menu's exit to Windows runs the teardown and then posts
`WM_DESTROY`, and the game leaves a moment later through the CRT's `ExitProcess`
([the engine map](../exe-reverse-engineering.md#the-demo-recorders-capture-where-it-runs-and-what-it-reads-disassembled-measured-2026-09-29)),
which would kill a writer still writing its last block. The fork hooks `ExitProcess` in the IATs of
the exe and of the DLLs in its folder, and its own exits, which that hook does not reach, call the
same wait, `tagpu_rec_exit`, before theirs: closing the window, `tagpu_refuse` (a saved game it
cannot load, a unit script that cannot continue) and the out-of-memory and unit-type refusals. It
waits for the writer of every recording that has ended, each of which sets an event once its file
is closed. A crash exit calls `TerminateProcess` and does not wait, since the crashed thread may
hold a lock a writer needs. A writer never waits for another thread, so the wait ends when the
files do. A recording still in progress at the exit — the window closed mid-game — has no end to
wait for, and its file reads to its last block, incomplete. A recording takes one of eight slots
for its event at its start and does not start without one, so its end always has a place to hand
it to.

Besides the teardown, a recording ends marked incomplete when the ring is full, the file cannot be
written, a table's growth finds no memory, the unit array moves or changes size or GameTime goes
back without a teardown, or the game stops being a skirmish; the rest of that level is not recorded. A load from
the in-game menu runs the teardown first, so the recording in progress ends complete and the loaded
level starts its own at its first iteration. A loaded game starts paused, and a paused game has no
iterations to record.

### Where the code is

- `src/tagpu_recorder.c`: the capture, the start and the end, and the harness's `dump` verb. It
  names engine memory, so it has a line in `thread-split.allow`, `publisher presentation`, approved
  by the owner on 2026-09-29: `presentation`, because in S1 the recorder holds no state the
  simulation reads. S2 revisits it for the puppet-mode flags COB reads.
- `src/tagpu_recwriter.c`: the writer thread. It is not on the list, so the build refuses an engine
  address in it, and "the writer never reads engine memory" holds by construction.
- `inc/tagpu_recorder.h`: the ring, the hand-over and the wake rule. `inc/tagpu_recfmt.h`: the ring's
  records and the file's blocks, shared by both files and mirrored by the tool.
- B2's stub at `0x4954ED` (`tagpu_patches.c`) calls the capture before the air pool's rebuild, and
  `tagpu_reclaim.c`'s teardown wrap calls the recorder's end first.
- The exit's wait: `fake_ExitProcess` (`winapi_hooks.c`, in `hook.c`'s kernel32 list, local
  modules only) and the fork's own exits — `SC_CLOSE` (`wndproc.c`), `tagpu_refuse.c`, and
  `hit_no_memory` and `lim_types_refused` (`tagpu_patches.c`) — call `tagpu_rec_exit` first.
- `tagpu_simreg`: a walk of the declarations and of the levers for the recorder, and
  `MEM_WRITE_WATCH` on every heap table `SIM_ALLOC` takes.
- `src/tagpu_reccodec.c`, `inc/tagpu_reccodec.h`: the stream codec — the sectioned container, the
  unit and pool frames and the zstd packing — and `inc/tagpu_recframe.h` with
  `src/tagpu_recframe_cells.c`, `_orders.c`, `_events.c` and `_state.c`, the record frames: portable
  C99 with no Windows call, no engine address in code and no writable static, used by the writer,
  the reader and `tests/codectest.c` (`make codectest`;
  `tests/codectest.py` transcodes, decodes, fuzzes and compares C with Python, natively under ASan
  and UBSan with `--native`). `src/zstd.c`, `inc/zstd.h`, `inc/zstd_errors.h`: zstd 1.5.7, vendored
  (`tools/zstd/amalgamate.sh` rebuilds them from the release tarball). `replay-lanes.tsv` and
  `tools/replay-lanes-gen.py`: the unit frame's lanes; `replay-layout-check.sh` runs the
  generator's `--check` on every build.
- `tools/replay/tarec.py`: reads a recording — `info` (the header, the blocks and how it ended),
  `check` (the stream against every snapshot the file carries) and `compare` (the stream to a
  dump's GameTime against the dump, byte for byte). Its block walk is `tools/replay/reccodec.py`'s,
  which needs numpy, numba and zstandard: `tools/replay/devpy.py` re-executes a tool under the
  shared venv (`.venv-undither/`) when they are missing — `tacli replay forks` and
  `replay takeover --dry-run` call it before they walk a file's blocks — and `header_only` reads a
  stored header with the standard library alone, so the build's bare `python3` still runs
  `tareplay.py features --check`.
  Decoding a large battle in Python takes about 3 s; with `TAREC_CACHE=DIR` the decoded blocks are
  kept in `DIR`, keyed by the SHA-256 of the file's bytes and of the decoder's sources, so a second
  command on the same file reads them back (`tools/replay/reccache.py`: an entry carries its own
  checksum and anything wrong in it is a miss). `tareplay.py` points it at its run folder and removes
  it at exit.
- The verb `dump <name>` in `tagpu_replay.trigger` (read by the replay's frame visit since S3b, which
  also takes `dump <name> at <T>`; *S3b — the globals, and seeking*): the capture point that takes it
  writes the live world's images to `replays\dump-<name>.bin`, a stored snapshot of the state that
  capture records.

The switch is a file, not a setting: `tagpu_recorder.on`, read with `tagpu_opt_on`, which
`tacli arm <instance> recorder.on` writes. It decides whether a game is recorded, not what the
simulation runs, so it is not a simulation lever and the header's lever list does not name it. The
product's opt-out is a setting, and the settings store's only writer is its menu
(`tagpu_settings.h`), so the setting comes with its menu row in S5.

### Verified by

Scripted games on Wine, on a private X display, in the raised build unless stated; each recording
read back with `tarec info`, `check` and, where the run took dumps, `compare`. Runs 1 to 5 ran on
S1 as the landing review read it. Runs 6 to 8 and the row after them ran on the build that lands
but for the file that asks for a recording, which changes only whether one starts; that build
changed every ID by one, and so every image, and added the exit's wait. Runs 9 and 10 ran on the
build that lands, run 10 with the two scratch changes its row names, and the last row on local
`main` after the landing:

| run | the game | what it showed |
|---|---|---|
| 1 | three stock AIs and an idle human seat on Core Prime Industrial Area, eight minutes, three dumps | 14,610 iterations and no correction; every dump equal (26, 39 and 64 live units), and all 16 snapshots equal to the stream |
| 2 | the experiments' battle on Town & Country, 1,428 units applied, two minutes, three dumps; run twice | no correction and every dump equal, 1,171 to 1,407 live units at the dumps; the cost below |
| 3 | run 1's game saved a minute in and loaded from the in-game menu, then played a minute, two dumps | the recording in progress ended complete at the load, at GameTime 1846; the loaded game started paused, and once running recorded from GameTime 1847; both dumps equal, no correction |
| 4 | run 1's game, the process killed with SIGKILL two minutes in | the file said incomplete and read to GameTime 3601, its four snapshots equal to the stream; copies cut inside a payload and inside a block header, and one with a torn block header, read to the block before the damage |
| 5 | run 1's game in a scratch build, not landed, that flips one byte of the writer's copy of a unit, a player and a declared table at GameTime 1000 | each correction named in the log and in the end block: the player at GameTime 1001, the unit at 1018, `g_def` row 0 `+0x8` at 1032; the dumps equal |
| — | run 1's game for 90 s in the stock-limits build | 22 tables, every dump equal, no correction |
| 6 | run 1's game left a minute in by the in-game menu's `EXIT`, `EXITGAME` and yes, in a scratch build, not landed, whose writer holds three seconds before finishing | the recording ended at the teardown; the exit logged that it waited for one file, the writer closed it, and the process went: the file complete, its dump equal, no correction |
| 7 | run 1's game ended by a defeat a minute in, and its window closed at once (`WM_DELETE_WINDOW`, which Wine turns into `SC_CLOSE`), in the same scratch build | the same: the exit waited for the file, complete, its dump equal |
| 8 | run 6 in the landing build | the writer had closed the file before the exit came, so nothing waited: complete, its dump equal |
| — | run 2's battle twice, and run 1's game for 90 s in the stock-limits build, all in the landing build | no correction and every dump equal: 1,099 to 1,404 live units at the battles' dumps; 22 tables in the stock-limits build |
| 9 | run 1's game without `tagpu_recorder.on`, then with it and left a minute in by the in-game menu | without it, `recorder: not recording: tagpu_recorder.on is absent` and no file; with it, recorded from GameTime 1 and complete at the teardown after 1,964 iterations, both snapshots equal to the stream, the dump equal, no correction |
| 10 | run 1's game ended by a defeat a minute in, and at once a refusal through `tagpu_refuse`, in a scratch build, not landed, whose writer holds fifteen seconds and whose frame hook refuses when a trigger file appears | the refusal came a second after the level's end and its box was closed a second later; the exit logged that it waited for one file, the writer closed it when its hold ran out, and the process went: the file complete after 2,130 iterations, its dump equal, no correction |
| — | runs 3 and 4 again on local `main` at `45f01501`, 15,001 unit slots | run 3: the recording in progress complete at the load at GameTime 1845 and the loaded game's recorded from 1846, both dumps equal (GameTime 2848 and 3776), no correction; run 4: incomplete, read to GameTime 3601, its four snapshots equal to the stream; the copy cut inside a payload read to 2101, and the copies cut and torn in a block header to 901 |

### Measured

The capture's cost per iteration, by `QueryPerformanceCounter` around it, on the reference setup
under Wine, in the build that lands:

| game | the first iteration | the rest, on average | the rest, at worst |
|---|---|---|---|
| run 1's game, a minute (runs 6 to 8), up to 14 live units | 2.3 to 2.9 ms | 84 to 91 µs | 204 to 552 µs |
| run 2's battle, twice, 1,099 to 1,404 live units at the dumps | 2.5 and 2.7 ms | 255 and 247 µs | 1.16 and 1.48 ms |
| run 1's game in the stock-limits build, 90 s | 0.6 ms | 90 µs | 373 µs |

S1 as the review read it measured the same rest: 78 µs on average over run 1's eight minutes (at
worst 760 µs), and 251 and 235 µs in two battles (at worst 583 and 610 µs). Its first iterations
took 4.3 to 4.8 ms: `NULL`'s ID was `0xFFFFFFFF`, so a row of zeros with an address in its layout
imaged to something else, and every such row went into the ring. The first iteration images every row of every table, the
extra weapons' 16,384 unit-type rows of 140 bytes among them; a row of zeros now costs it a
comparison and nothing in the ring. In the stock-limits build, whose unit-type tables are 512 rows,
it costs 0.6 ms.

The product page's budget is 0.5 ms on average and 1 ms at worst with 1,500 units. The average
meets it. The worst did not in three battle runs of five: 2.3 ms in a run while S1 was being
built, and 1.16 and 1.48 ms in the two above. The log names the costliest iteration's GameTime and
what it put into the ring, and the 1.48 ms one put 26 KB, about half what an average battle
iteration puts there: the stream averages 20 KB an iteration (75,590 KB over 3,840), and the
rotating check adds at least 18 KB and about 30 KB on average at the battle's 1,100 to 1,400 live
units, so the time was not the capture's own work. The cause is not found; the writer's wake
signal, a round trip to the wineserver under Wine that falls on about one battle iteration in
twenty, is a candidate, and the reference setup was running other work.

One cost falls outside the timer. Under Wine a write watch is a page protection, so after each
reset the simulation's first write to a watched page takes a fault in the engine's own code; a
scratch program measured 4.2 µs a fault under Wine 9.0 on the reference setup, 5.2 µs with three
other threads busy. The log counts those pages after the first iteration: 393 and 269 in the two
battles' 3,839 iterations, at most 3 in one, so under 20 µs in the worst iteration and about half
a microsecond on average; 6 to 9 in the four-AI games.

Size, the exact stream with every value at every tick: run 1's file is 2.0 MB for 8.1 minutes,
14.8 MB an hour at that rate — its stream deflated 3.4:1 to 1.7 MB, and its 16 snapshots, mostly
zero rows, add 0.26 MB — against the budget's 25 MB for a four-player game, which a longer game with
more units would pass. The two-minute battles came to 18.5 and 19.8 MB a minute in the reviewed
S1 and 20.3 and 17.7 MB a minute in the landing build (the stream deflates about 2:1 there),
1.1 to 1.2 GB an hour if sustained, against the budget's 100 MB for ten players with 1,500 units.
The ring held at most 645 KB of its 16 MB in the battles and 177 KB in the one-minute games; in
the reviewed S1 the first iteration's rows of zeros alone put about 1.2 MB into it.

A scratch repacking of the landing build's battle — one record an iteration rather than one a
unit, each value stored as its difference from the unit's last, and each field's differences stored
together — deflated to 13.8 MB, against 34.5 MB for the same stream as written: 2.5 times smaller,
6.5 MB a minute. The differences are most of that: one record an iteration alone gains 1.2 times.
Run 1's eight-minute stream, recorded by the reviewed S1, came out 2.7 times smaller. The repacked
stream was not decoded back, so this measures what the representation gains, not a format.

### Gaps S1 leaves

- **The objects a record points at** — the mover, the order lists, the COB object, the model, the AI
  object, the control groups, the sight counters and the resource accumulator — are not recorded:
  their rows are `object` and zero in the image. S2a brings the mover, the order lists and
  PlayerInfo, with layouts of their own (*S2*, the rows decided 2026-09-30).
- **Memory a declaration reaches through a pointer** is not recorded: the engine pools the DLL holds
  (`SIM_ENGINE`: explosions, particle systems and their auxiliary geometry), the blocks a row owns
  (`SIMF_OWNED`), and the columns of a struct-of-arrays table (`SIMF_COLUMN`: `s_hit`'s `stamp`,
  `freed` and `local`, one entry per unit slot, whose row alone is recorded; S4a records the three
  as tables of their own).
- **The header** has no game options and no fingerprint of the weapon definitions: S2a. Alliances
  are values of the player records, so the stream carries them.
- **The worst iteration** met the 1 ms budget in two battle runs of five, and its cause is not
  found (*Measured*).
- **The recorder's memory** is not measured. It is bounded by construction — the 16 MB ring, and
  shadows and a copy sized by the unit array and the declared tables, fixed at the start but for the
  side table's one growth — against the product page's starting point of about 64 MB.
- **Events** (shots, script starts, sounds, the nano spray): S2a.
- **Features, wrecks, the main block and `.data`**: S3.
- **Network games** are not recorded: the multiplayer slice.

## S2 — watching

Planned 2026-09-30 from four research tracks, whose evidence landed with S2a:
[the mover and carried units](demo-recorder-s2-movers.md), [the order lists](demo-recorder-s2-orders.md),
[the events](demo-recorder-s2-events.md) and [the puppet host](demo-recorder-s2-host.md). S2 is two landings. **S2a** records
everything watching needs and verifies it against the engine; **S2b** plays a recording on a
puppet host and verifies it against the recording.

### S2a — what watching needs, recorded

- **The mover.** A mobile unit's movement state is one `0x2F`-byte object (the creates' `push 0x2F`,
  `0x4860C4`), made with the unit and freed with it. Its bytes therefore ride as a twelve-dword tail
  on the unit's image, and the unit image's `+0x00` says whether the unit has one. Three rows; every
  other byte is a value:
  - `+0x00`, the movement class object, `drop`: its kind depends on the machine, a local class on
    the recording machine and a remote proxy on a puppet host;
  - `+0x04`, the movement class, `cache`: written only by the constructor, from the unit's type;
  - `+0x2E` bits 3–7, `drop`: allocator leftovers that every writer masks out and nothing reads.

  It adds about 14 KB to a battle iteration (*S2a, verified and measured*): the moved-at stamp
  `+0x2A` holds GameTime on every tick a unit moves, and the speed and the velocity change with it.
- **The order lists.** Each unit's main list `+0x5C` and sub list `+0x60`, recorded as value images,
  a list whole whenever its image differs from the recorder's own previous one. A node image holds the action, state,
  events, deadline, the target as an ID, position, leash, the three parameters, flags, issue time,
  fired events, and the owned object's class code. A node's address is not recorded: playback
  rebuilds a puppet's lists from values. The node's rows:
  - `+0x0E` (the list's unit), `+0x1A` and `+0x1E` (the target reference's links): `cache`;
  - the draw's last-seen target position `+0x32`/`+0x34` and its valid bit `+0x42` bit 21: `drop`;
  - the four fields that arm owner code when a node is destroyed: `puppet0` and `puppet1`
    (*Decided*).

  A list longer than 4,096 nodes is a named failure that marks the recording broken from that tick.
  It is never truncated.
- **PlayerInfo**, the `0xB9`-byte block a player record points at (`+0x27`). It holds the side,
  logo, share bits, the watcher bit and the LOS bits. The radar pass and the draw read it, and the
  console's `Share*` commands change it in play. It is recorded as a record of its own.
- **Events at the engine's own roots.** Every call is recorded, nested ones included, with its
  step and its order in the step. A nested pair is inert on its second run by an engine test: a
  compare, an occupied slot, or an alive test. The roots:
  - **Create**, at `0x485F50`'s success exit `0x4861B3`, which sees the slot actually taken.
  - **Death**, at `0x4866D0`'s entry, with the victim's record as the destructor finds it. The
    reaper runs after the unit's own movement step, so image T is a step early for the wreck's cell
    and the blast.
  - **Shots**, from the `0x0D` packet of the five fire functions, with their network tests released
    while recording; and **interceptions**. The event's start (`+1..+0xC`) is the one the build
    used: `0x49D9C0` sends the shooter's centre there instead, so for its shots the recorder takes
    the build's start from the send's frame and leaves the packet sent as it is
    ([engine map](../exe-reverse-engineering.md#what-a-shots-packet-says-of-its-start-0x49d9c0-0x49d270-disassembled-measured-2026-10-09)).
  - **Script starts** through the three broadcasters `0x456190`, `0x456200` and `0x456290`.
  - **Build done**, `0x41B8D0`, which clears the model's composite frame `[+0x9E]+0x10` (the
    cached 1x bake), so a finished unit bakes again as a finished one.
  - **The state byte** `+0x10E`, as its new value, from `0x48B090`.
  - **Unit replies**, at `0x47F780` and `0x47F850` before their viewer test, for every player.
  - **The script starts only the owner's machine makes**: `HitByWeapon` and `TakeDamage` in the hit
    `0x489CE0`, `TransportPickup`, `TransportDrop` and `EndTransport` in the transport orders, and
    `TargetCleared`. A replay host applies no damage and runs no order handler, so it never makes
    these itself. `BeginTransport` is broadcast, so the broadcasters record it.
  - **The nano spray**, at its emitters (the completeness note's *Decisions taken*).
  - **A controller change.**
- **The header** gains:
  - the game options as the simulation ran them: the LosType bits, ActiveCommanderDeath, the
    difficulty and the start-position mode;
  - each seat's PlayerInfo and alliance row at the start;
  - the recording viewer, `+0x2A42` and `+0x2A43`;
  - the map's settings name `[main+0x29A0]+0x11C`, which is what `SkirmishMap` holds (`RT_MAP` is the
    TNT's stem, which a map can name differently);
  - the player count `+0x2A3C`;
  - a fingerprint of the weapon definitions.
- **Verified by:**
  - S1's checks over the new records: self-verification, and the dump compared byte for byte with
    the recording read back to its tick, now with every mover, order list and PlayerInfo;
  - the events checked against the images, offline: every unit that appears has its create, every
    unit that disappears its death, every `+0x10E` change its state event, and every finished build
    its build done;
  - the cost and the size, measured again.

#### S2a, verified and measured

On the reference setup under Wine, 2026-09-30, in four scripted games: an eight-minute skirmish of
four AIs on Core Prime Industrial Area (14,610 iterations), run 2's two-minute battle of 1,432 units
on Town & Country (3,840), and in the stock-limits build a three-minute skirmish of four AIs
(5,610) and a one-minute run of the battle (2,010). In each:

- no correction: the self-verification now covers every mover, order list and PlayerInfo;
- every dump EQUAL to the recording read back to its tick (three a game: 16, 27 and 42 live units in
  the eight-minute game, 1,400, 1,309 and 1,206 in the battle, 14, 19 and 27 and 1,412, 1,409
  and 1,400 in the stock-limits games), and every snapshot equal to the
  stream;
- every change between two images explained by the step's events (`tarec.py check`): no appearance
  without its create, no disappearance without its death, no `+0x10E` change without its state
  event, no build finished without its build done;
- no ID outside its table, no event refused off the game thread, no nano spray uncredited, and no
  order node whose owner or list flag disagreed with the list holding it.

The events of the battle: 1,430 creates, 1,433 deaths, 7,780 shots, 20,632 broadcast starts,
21,873 owner-only starts, 61 state bytes, 16,415 replies and 24 sprays; of the eight-minute game,
48 creates, 51 deaths, 84 broadcast starts, 48 builds done, 56 state bytes, 170 replies and 16,601
sprays. The stock-limits battle recorded 568 shots and 3,464 owner-only starts, so the released
network tests and the shot callouts run in that build too. No game intercepted a projectile.

**The capture's cost** grew with what it reads. In the battle it took 475 µs an iteration on
average (S1: 247 and 255) and 1.15 ms at worst, in the 37th iteration, while the scenario's 1,432
units were being placed (710 KB into the ring); in the eight-minute game 144 µs on average and
455 µs at worst, and in the stock-limits game 96 µs and 365 µs. The stock-limits battle's one
minute, with 1,400 to 1,412 units alive throughout, took 528 µs on average and 1.57 ms at worst.
The product page's budget is 0.5 ms on average and 1 ms at worst with 1,500 units: the battles'
averages sit at it (475 and 528 µs), and their worst iterations pass it.

**The size.** The battle came to 60.4 MB for two minutes, 30 MB a minute (S1: 17.7 to 20.3). Its
stream is 38 KB an iteration before deflating (S1: 20 KB): the order lists 4.1 KB, the events 0.7
KB, and the rest, about 14 KB, the mover tail [INFERRED: the growth the lists and the events leave;
PlayerInfo changed in no iteration after the first]. The eight-minute game came to 1.34 MB, whose
order lists are two thirds of the stream before deflating (6.7 of 10 MB) and deflate away: the
stream deflates 8:1. A list goes whole whenever a node changes, and the build and repair states,
self-repair, resurrection, capture and the reclaims re-arm their node's deadline `+0x0A` every tick
(`0x439E80`), so such a list goes
every tick. A delta of a list's dwords would gain little: the lists are about a tenth of the battle's
stream, and deflating removes their repetition in the smaller game. What the size needs is the
representation's change S1 measured (2.5 times smaller), the product page's open decision 2.

**PlayerInfo** is cheap: at most five of its `0xB9` bytes are nonzero in a seat of the battle, and
none changed in play. **The ring** held at most 1,059 KB of its 16 MB in the battle (S1: 645 KB).

#### What S2a leaves

- **An interception** has not been recorded by a game: none of the three had an interceptor.
- **The capture's cost** in a battle is at the 0.5 ms average budget (475 and 528 µs) and past the
  1 ms worst (1.15 and 1.57 ms). S1 took about half as long; what S2a's reads cost is not broken
  down. A one-minute game of eleven units on the landing build had one iteration of 2.6 ms that
  put 18 KB into the ring: S1's worst iteration, whose cause is not found (*Measured*).
- **The size**, 30 MB a minute in the battle: the representation (the product page's open
  decision 2).
- **The game options change in play** through the typed commands `LOSType` (`0x416690`), `LOS`
  (`0x416D50`) and `Mapping` (`0x416D80`): the header records them at the start, and S3b records
  LosType bits 0–2 at every iteration and plays a change with the engine's own reaction.
- **A seek from a snapshot** takes the events of the step after it from the stream block before
  it: the writer writes the snapshot at T after the block that ends with step T's events
  (`tagpu_recfmt.h`, `RB_SNAPSHOT`); S3b's `recr_seek` reads it so.
- **An order node's owned object outlives the node's hold only for a type-3 owner**: that owner's
  air proxy (vtable `0x4FD9E0`) deletes the object an install hands it (`0x490A10`). A skirmish has
  no type-3 owner, and a puppet holds `+0x52` at 0 (`puppet0`), so it matters to the takeover (S4)
  and to the multiplayer slice, whose remote units have type-3 owners.

### S2b — watching

Built as planned, but for what *As built* marks. The engine facts are
[the engine map](../exe-reverse-engineering.md#the-puppet-host-its-gates-the-30-tick-deadline-and-the-sight-loop-disassembled-measured-2026-09-30)'s,
the evidence [the host note](demo-recorder-s2-host.md)'s, and what the module writes
[GPU status](../gpu-status.md) §2.6f.

- **Opening a recording** goes through the engine's own SKIRMISH Start.
  `tacli replay play <instance> <recording> [--perspective RECORD]` writes the instance's skirmish
  settings from the header, copies the recording into `replays\`, writes the request file
  `tagpu_replay.play` (`play <name> [<record>]`), and clicks Start. Four sites are installed with
  the others, inert unless a request armed the level:
  - **The seat fill's call `0x47B092`.** It validates the request before any write: every puppet
    gate and the teardown wrap in, the format, the layout's hash, a skirmish, no network, the map's
    settings name, two to nine seats, as many players as seats, each seat's side and colour within
    this game's counts, the unit limit and the view. Then it seats each recorded
    player as type 3 through `0x464290` with its side, logo, name and alliance row, seats the
    viewer, points `+0x2A43` at the view, and disarms the victory and defeat tests (`0x4904B0`).
    On a failure the log names the reason and a normal skirmish starts. A recording from another
    build plays, and the log names both builds (D8).
  - **The settings persist's call `0x47B09C`.** It applies the recording's game options after
    `0x430F00` has written the player's own to the registry. The teardown restores them. From the
    level's first step, when the loader has read them, an observer on `0x430F00` puts the player's
    options in for the length of every save, so a save made during the playback (Options' OK at
    `0x45FD5F`, the damage-bars key at `0x496077`) writes the player's, and the save runs and
    returns unchanged.
    A save before the first step, which no known path makes, writes the recording's, and the
    teardown then saves again.
  - **The start units' calls `0x497963` and `0x497ACD`**, skipped.
  - **The level init's call `0x4919C8`**, retargeted: after `0x464990`, record 10's buffers through
    `0x464700`, on the loader thread ([the ten-seat note](demo-recorder-ten-seats.md) §4.2).
  - **The first step**, at B2's stub `0x4954ED`, at its capture point before the unit tick: the
    level's counts, map, unit and weapon data and first GameTime against the header; then the
    start units through the create, the first image, the wind's seed and V's sight. Made there,
    the start units get the script slice the recording machine's first unit tick gave them
    (`tagpu_replay.c`, THE FIRST STEP'S UNITS).
- **The viewer's seat** V is player record 10, the engine's no-player record, which no recording
  carries ([the ten-seat note](demo-recorder-ten-seats.md)): type 3, with the watcher bit and an
  empty block, holding `+0x2A42` for the whole level. The record and its PlayerInfo go back as the
  Start found them once the teardown `0x491B60` has returned, after the post-game table has left
  the watcher out:
  the seat activation does not clear the watcher bit, and a surrender to the main menu passes no
  reset (MEASURED).
  - Every path that selects or orders walks the controlled player's units, of which this seat has
    none, so the viewer cannot select or order a puppet unit.
  - Four gates cover the rest of the viewer's input: typed commands (`0x417B50`, every one
    refused), the debug keys (`0x4956C0`), the resource transfer (`0x464C60`) and saving
    (`0x4326B0`).
  - A recording may seat all ten records: the viewer's seat is record 10, outside them.
- **What the viewer sees.** Without `--perspective`, everything: V's sight counters are filled, V's
  bit is set in every cell of the explored map, and the radar pass marks every unit for a watcher.
  With `--perspective N`, `+0x2A43` is record N and the host's own radar pass computes N's radar
  picture, the recording's own where the recording showed N (`tarec info` names the recording's
  viewed player). V's sight is filled either way, so switching the perspective is `+0x2A43` and
  LosType bit 3 alone, for S5's selector.
- **The step.** B2's stub calls the host's unit tick, then the replay: the events of step T in
  recorded order, then the image of T+1, the world the recording machine's unit tick and the rest
  of its iteration T left. The rest of the host's iteration then reads what the recording
  machine's read. *As built:* the plan put the image before the host's unit tick, which left the
  sight stamps of 15–20 of 1,413 units a tick behind at every dump.
- **The apply** runs each step on the game thread. Every value is bounded before it becomes an
  index or a pointer. The completeness note's *What restoring these records needs* is its
  checklist.
  - **Units are placed with the engine's set-position `0x48A9F0`** at the recorded position and
    the altitude the movement pass had before its fix-up. Then the walk-class and occupancy parts
    `0x43DA70` and `0x43DB50` run, which start the walk and occupy scripts, and the fix-up's values
    go in. `+0x110` bits 2–3 and `+0x10A` are written only by those parts, and compared. The
    placement part `0x43D6D0` and the altitude fix-up `0x48A870` never run on a puppet.
  - **Players**: every record but V's, raw under the exclusion file, but the players phase's
    30-tick deadline `+0xF0`, which the host keeps (*as built*: written, it arrived already
    advanced, and the host never ran the viewed player's radar pass; the host note §2.3). An image
    goes in only with both names ended inside their 0x1E-byte fields, and a seat's PlayerInfo image
    only with its side and colour within this game's counts; the engine copies a name to its end and
    indexes a seat's side and colour tables unbounded, and itself leaves colours outside its count
    in the records it does not seat. Every seat's `+0x18` is GameTime each step, for
    the pacing gate.
  - **A unit's `+0x110`** takes the image's bits less the engine's and less the exclusion file's
    rows, so the viewer's selection, marks and build page are the viewer's own.
  - **Carry links** follow the recorded chains through the engine's attach `0x48AB70`, bounded
    first. They are never written raw.
  - **Order lists** are reconciled node by node against their images through the engine's
    constructor `0x43A0C0`, linker `0x43AD10`, reference set `0x489690` and destructor
    `0x43A1F0`, with every node's puppet rows held.
  - **Events are replayed through the functions that made them:** a create through `0x485F50`
    into the recorded slot; a death through C4's wrap of the destructor, after the victim's
    recorded fields; a shot through the receiver `0x49D270`, which builds from the recorded start,
    so the projectile starts where the live one did; a script start through `0x4B0B00`,
    `0x4B0A70` or `0x4B0940`; a build done through `0x41B8D0`; a state change through `0x48B090`;
    a reply through `0x47F780` or `0x47F850`, whose own viewer test plays the watched seat's; a
    nano spray through `0x4720D0` or `0x472200`. What a create nests is recorded before the create
    and finds no unit; the replayed create makes it again. Each event is counted as played or not,
    and the log names the classes of those not played.
  - **A recorded YARD_OPEN** (`RK_EV_YARD`, written at YARD_OPEN's write `0x47DAF1`) is applied at
    its place in the step's events, through the re-claim `0x47C790` and the passability refresh
    `0x440A40`: a lab's script slice can open its yard and its build handler make the nanoframe in
    it later in the same tick, and the create finds the cells free only after the yard. A unit the
    first step or a seek makes gets its recorded yard as soon as its create returns, before the next
    slot's create stamps: an open yard gives no cell back to a unit that lost it to the closed
    structure. After the positions, the image's yard goes in where the host's differs; that is
    how a yard event refused for naming a unit its own create had not yet made reaches the host
    ([engine map](../exe-reverse-engineering.md), *An open yard gives nothing back* and the two
    paragraphs after it).
  - **The wind** is seeded once, from the recording's first image, so every draw after it is the
    recording's.
- **The recording is the only writer.** On a puppet host, twelve prologue gates
  (`tagpu_puppet.c`), with the viewer's four above:
  - damage is refused at the apply `0x489CE0`, where every hit lands, for a puppet's unit
    (*as built*: the plan put it at B3's drop);
  - `Send_UnitDeath 0x4864B0` is refused for a puppet's unit, so bit 14 is written raw and a dying
    unit lives until its recorded death (*as built*: the plan had the reaper skip puppets);
  - COB's SET, ATTACH and DROP and the two replies are refused for a puppet's unit, except inside
    the replay's own create and reply;
  - the host's meteor shower does not run;
  - every order node on a puppet is held disarmed (`puppet0`, `puppet1`).
  - *As built:* Impure's per-slot hooks, B4's stamps and C4's marks, run on puppets unchanged,
    where the plan had them skip puppet slots. Each writes from the slot, GameTime and its own
    previous value, at the creates and deaths the replay makes at their recorded points, so its
    table agrees with the recording's; S2b writes no declared table but the wind's, and a hook
    that skipped puppets would have left its table differing.
- **Getter 75** answers, for each puppet seat, the recording machine's bit, from a per-seat table
  built from the header at the seat fill, declared `SIM_CONTENT` in `tagpu_puppet.c` and constant
  for the level. The recorder refuses to record a puppet host.
- **The dumps and the comparison.** `dump <name>` in `tagpu_replay.trigger` writes the live world
  with a header naming its tables, and `tools/replay/tarec.py compare --puppet` compares it with
  the recording read back to its GameTime: every unit, player and order list image and every
  declared table, paired by name, since each build numbers its tables in its own order; less the
  contact bits, the controllers and the viewer, and with the contact bits when the dump's viewed
  player is the recording's. V's record, record 10, is in neither the recording nor the dump.

#### S2b, verified and measured

On the reference setup under Wine, 2026-09-30, a dump every 8 or 12 seconds of play, each compared
with the recording read back to its GameTime (`tarec.py compare --puppet`):

- **The 1,432-unit battle** on Town & Country (3,810 iterations), recorded by an earlier build of
  this landing, so also a recording played by another build (D8). In full visibility, and from
  record 0, the recording's own viewer: every one of the nine dumps inside the recording equal
  (GameTime 384 to 3,712, 1,413 live units down to 1), units, players, order lists and every
  declared table, and from record 0 the radar picture too. The events played kind by kind: 1,430
  creates, 1,433 deaths, 7,175 shots, 19,051 broadcast starts, 19,341 of 19,600 owner-only starts,
  1 build done, 37 of 62 state bytes, 16,780 of 16,805 replies and 459 sprays; not played, 25 state
  bytes and their 25 replies nested in a create, which the replayed create makes again, and 259
  TargetCleared starts nested in a death, which the destructor's order purge starts on the unit it
  destroys and a puppet's held nodes do not. Refused on puppets: 252 `Send_UnitDeath` calls, the
  reaper's on units the recording marked dying [INFERRED: its other callers need a network game or
  a type-1/2 owner], 26 SETs, 37 replies and 3,809 meteor showers, one an iteration; no hit reached the apply, since a puppet's shots do no damage by the impact's owner
  gate (B7).
- **The transport game** on Town & Country (3,090 iterations: two Atlases loading, dropping and
  reloading Peewees), recorded by the landing build, whose recorder took its replies through the
  reply gates: `tarec check` clean, and in full visibility and from record 0 every one of the ten
  dumps inside it equal (GameTime 288 to 2,880), the radar picture too from record 0. Every event
  played but one state byte and its reply, nested in a create; refused, 6 SETs, 7 replies and
  3,089 meteor showers.
- **The stock-limits build** recording a four-AI game on Core Prime Industrial Area (2,010
  iterations, 0 corrections, `tarec check` clean) and playing it in both views: every one of the six
  dumps inside it equal, the radar picture too from record 0; refused, 16 SETs, 8 replies and 2,009
  meteor showers.

**The landing review's fixes**, verified the same day. The battle again, in both views: every one
of the four dumps inside it equal (GameTime 800 to 3,200), the radar picture too from record 0, and
the same counts as above. The transport game with the player's registry set apart from the
recording's options (commander death 1 and start location 0 against the recording's 0 and 1): after
Options' OK during the playback the registry held the player's values, where a build without the
observer wrote the recording's (the same clicks, the store rewritten with 0 and 1); V's watcher bit
was set during the playback (`+0x9B` 0x54), and a build that logged the restore showed it put back 0
after the teardown of a surrender to the main menu, a path that runs no reset of the records.
The reader's check of an order list cut short at an iteration's end was verified by reading only:
no malformed file was built for it.

**What the checkpoints caught.** From record 0 the host at first showed a radar picture of its
own: every unit had the bits its create gives it, bit 9 on record 0's 352 and nothing on the other
1,061, where the recording had bit 8 on all of them. The host had never run the viewed player's
30-tick pass, whose deadline the apply wrote already advanced; it now leaves that deadline to the
host (the host note §2.3). And the dumps named their tables by index, so a recording from another
build compared the wrong tables; a dump now carries a header naming them.

### Decided 2026-09-30

- **S2 is two landings**, S2a and S2b.
- **The mover's and the order node's rows above are approved**, with a new class, `puppet0` and
  `puppet1`. Such a field is recorded and compared off a puppet. On a puppet it is held at 0
  (`puppet0`) or set (`puppet1`), and a takeover restores it. The class covers the order fields that
  arm owner code when a node is destroyed:
  - `+0x06` bit 1, the handler's own call, held 0;
  - `+0x42` bit 22, a `StopBuilding` owed, held 0;
  - `+0x52`, the handler's owned object, recorded as its class code, held 0;
  - `+0x42` bit 16, set when the destruction resets no weapon target, held set.

  A replayed death, attach or teardown then only unlinks and frees a puppet's nodes, and never
  runs owner code a second time.
- **On a puppet host the engine computes the contact bits for the viewed player.** The host's
  radar pass runs for `+0x2A43`. The recorded bits are kept for a takeover, and compared when the
  recording viewer's perspective is shown. This amends 2026-09-29's "a replay shows the recording
  viewer's radar picture": that perspective still shows it, computed by the engine from the same
  inputs, and every other perspective shows its own player's radar, as the product page has it.

- **`tagpu_replay.c` is `presentation`, `tagpu_puppet.c` `sim` and `tagpu_recreader.c`
  unlisted** (the owner, "two files"). The replay writes engine state only through the
  engine's functions and the recorded images, and keeps no simulation state of its own; the puppet
  table and the gates' own-calls flag are `tagpu_puppet.c`'s, declared there.

### What S2 leaves

- **Selecting a puppet unit**, its order markers and a factory's queue: S5's perspectives and
  overlays.
- **Features, wrecks and the footprint grid's contested cells**: S3. Until then a replay's features
  change only by the replayed deaths' wrecks [INFERRED: a host applies no damage and runs no order
  handler, so its trees do not burn or fall and nothing is reclaimed].
- **The path a mover's class object holds.** A takeover plans a new one from the orders (S4).
- **Recordings that seat all ten records.**
- **The declared tables are compared, not written**, but for the wind's generator, seeded at the
  first step. B4's stamps and C4's marks agree because their hooks run on puppets as they ran
  there; S3's restore writes the tables.
- **The playback's own cost** is not measured.
- **The replay's statics are outside the simulation-state check**, `tagpu_replay.c` being
  `presentation`: a takeover (S4) that resumes play from a playback has to account for them.
- **A network recording's controller changes** are not followed: getter 75 answers the header's
  controllers for the whole level. In a skirmish the one in-play change, the console's AI toggle,
  flips types 1 and 2, which getter 75 answers alike ([the host note](demo-recorder-s2-host.md)).

## S3 — seeking

Planned 2026-09-30, once S2 was in, from two passes over the disassembly: the feature grid and the
wreck pool with every root that changes them, and the main block and `.data` with what the
simulation reads across iterations. Their facts go into the engine map with the landing that uses
them.

### How S3 is cut

Three landings, as S2 was two, each a finished unit verified by running it:

| Landing | Delivers | Verified by |
|---|---|---|
| **S3a — features and wrecks** | the map grid's cells and the wreck pool recorded every iteration, with the events at their roots; their changes replayed on puppets through the functions that made them | S1's and S2b's checks with the features in them: every dump equal to the recording, live and on puppets; the capture's cost and size |
| **S3b — the globals, and seeking** | the main block's and `.data`'s simulation fields (a positive list), the projectiles and the explored map recorded, and both images whole in every dump; the rebuild in place from a snapshot on a puppet host; `tacli replay seek` | a fresh process seeking to a dump's GameTime is equal to that dump; forward and backward seeks; seek times against the target |
| **S3c — the round trip and the harness** | one writer for the declared tables on a puppet host, the puppet round trip and the rules against a vacuous pass, root, sink and COB-write counts, the null twin and the images gate, `tools/replay/tareplay.py` with its fixtures and its landing and broad tiers, the CLAUDE.md replay gate, the landing command's Step 4b | the round trip over the fixtures, none passing vacuously |

### S3a — features and wrecks

A recording carries the feature grid and the wreck pool, every call that changed them, and the
simulation RNG's draws those calls made; playback makes each change through the function that made
it. The engine facts are [the engine map](../exe-reverse-engineering.md#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30)'s.

**What the engine keeps.**

- **The grid**, `[main+0x14287]`: `W = [main+0x14233]` by `H = [main+0x14237]` cells of 13 bytes,
  row-major (`0x481550`), made per level by LoadMap (`0x48399D`) and freed at `0x483E7E`. It holds
  the features and the units' footprints together: the two unit slots, the height, the metal, the
  feature's def, an anchor's record or a footprint cell's way back to its anchor, and the flags.
- **The wreck pool**, `*(main+0x1420B)`: 2,048 records of `0x30` bytes in stock, 8,192 raised, on
  three lists linked by signed 16-bit indices: active `main+0x14213`, settled `+0x14217`, free
  `+0x1421B`. A record holds a 3DO feature for its whole life — a corpse, a heap — and a GAF feature
  while its burn, death or reclaim sequence plays. A free record's bytes are simulation input: a
  corpse starts with the velocity its record's last tenant left.
- **The writers.** A search of `.text` for every operand naming the grid, the pool, its heads or the
  cursor, and of every caller of the cell getters and the feature functions, finds in play: the
  features' bytes written by the spawn `0x423C50`, the destroy `0x4246B0`, FeatureDie `0x423550`,
  the burn `0x4233A0`, the damage `0x4244B0` and the swap `0x423710`, and the draw's flag bit 2;
  the unit slots and the yard bit by the stamp `0x47CC30`, the clear `0x47D0E0` and the re-claim
  `0x47C790`; the pool by those, the list move `0x4232F0`, the feature tick `0x424050` and the
  corpse `0x486360`.
- **The feature tick `0x424050`**, once an iteration at `0x495585`, after the unit tick, the
  projectiles, the explosions and the players phase, makes the features' only random decisions, at
  six calls of the simulation RNG `0x4B6C30`: reproduction, which draws for every GAF feature at
  rest its cursor passes (no stock feature reproduces), and the fire's spread and a burn's length.
  The rest is deterministic: the 3DO physics, the sequences' steps, a burn's end and a death's or
  reclaim's. Its smoke draws the CRT's `rand` and is presentation.
- **A death's explosion damages features on the owner's machine alone.** The owner gate
  `0x49A01B..0x49A047` passes by an explosion whose owner is a type-3 player, a picture; the corpse
  is made on every machine, after the explosion (`0x486D50`, then `0x486D6F`).

**What it records.**

- **The grid**, each cell's 13 bytes less the draw's bit 2: every cell at the level's first capture,
  handed to the writer whole through the ring (`RK_GRID`), then each cell that changed. The ten
  writers are observed at their entries, and each marks the cells it can write, a cell wider each
  way — a def's footprint at the spawn, the feature's at the destroy, FeatureDie, the burn, the
  damage and the swap, the unit's at the stamp, the clear and the re-claim — and the capture
  compares the marked cells with its copy. A rotating sweep compares a 64th of the grid each
  iteration as well: a changed cell that no writer marked is recorded when it finds it, late, and
  counted in the end record, which names a writer the search missed. Comparing the whole grid every
  iteration would be 7 MB on Two Continents, and a write watch would see the draw rewrite bit 2 on
  every drawn cell every frame.
- **The wreck pool**, compared whole each iteration (7 µs in stock and 26 µs raised, both copies
  cold, a scratch program on the reference setup): each record that changed, laid out by its def.
  A 3DO record's model instance is recorded as nothing (the replayed spawn makes its own), a GAF
  record's two sequence pointers as which of the def's sequences each is — the second, the shadow,
  only while the record uses it (`+0x2F` bit 2) and as none otherwise, since until then it holds
  what the record's last tenant left — and the rest raw. With it,
  the lists' three heads and the reproduction cursor, when any changed.
- **The calls that change them, at their roots.** A feature function called outside every other
  one is recorded at its entry with its arguments and the call's site, and at its return with its
  result where it has one (a record for the spawn and the swap, 0 or 1 for the destroy). The
  feature tick is a root at every call, and carries the cells that changed since the iteration's
  images, since reproduction reads a unit slot (`0x424189`). The six draws are recorded inside the
  root that made them. **A death is a bracket** from the destructor's entry to its return (one the
  capture's frame stack has no room for is not recorded, and ends the recording); the
  calls the destructor makes itself — the corpse — are its own and not recorded, and its explosion
  (`0x486D50`, redirected to a stub) is a frame inside which the area damage's calls are roots.
- **The header** carries `W`, `H`, the pool's size and the feature defs' count with a hash of their
  names; a capture that finds the grid, the pool or the count changed ends the recording
  (`INCOMPLETE: the features were not the ones it began with`). **Snapshots** carry every cell, every
  record that is not all zero, and the heads.

**Playback.**

- **The host decides nothing about features.** Seven more gates in `tagpu_puppet.c` refuse the
  feature tick and the six feature functions on a puppet level from its first step (before it the
  loader places the map's features through the spawn); each names the first call it refuses. A
  puppet's shots and deaths damage no feature by themselves (the owner gate), and its order
  handlers do not run.
- **Each root is made through its function where the recording made it**, with its arguments
  bounded — a cell by the grid, a def by the defs, a weapon by the weapons; FeatureDie and the
  damage index the grid with their `x` and `z` and never test the cell they get — and its draws
  armed, so the six calls take the recording's values while their site and argument agree. The
  tick's cells go in before the tick. **A death** is replayed as S2b replays it, with the feature
  gates open, so the host's destructor makes the corpse, and the roots its explosion made are made
  when the host's destructor has made its explosion, before the corpse. A result that differs, a
  draw not taken as recorded, a root out of bounds and a death whose explosion the host did not make
  are counted and the first named.
- **Then the images**: a cell's and a record's values go in raw; their structure — a cell's def,
  whether it holds a record and the record or its way back to the anchor, a record's links, anchor,
  def, flags and sequences, the lists' heads — is compared and never written, since only the
  engine's functions keep the grid, the pool and its lists consistent with each other. A value that
  indexes is bounded before it goes in: a unit slot by the unit array and by its unit being alive
  on the host, since the stamp's contest follows a slot to its unit's owner with no test
  (`0x47CDCA`); a sequence frame by the sequence's own count (as `0x4B8B30` bounds it), read
  through the def's field the recorded ID names and never through the record; the cursor by the
  grid's cells.
- **The unit slots and the yard bit come from the images alone** on a recording whose stream does
  not hold them; one that holds them (format 16's `RECL_CELLS`, every skirmish recording of this
  build) plays with the engine's own (*The cells held*, [making recordings smaller](demo-recorder-size.md)).
  The stamp, the clear and the
  re-claim still run on the host — for S2b's placements through the set-position `0x48A9F0`, the
  creates and deaths, and the host's own unit tick — because the stamp also files the unit in its
  sort bucket, which no image writes. Each call's writes to the grid are undone at its return: the
  recorder's observer at its entry hands the unit to `tagpu_puppet.c`, which saves the footprint,
  a cell wider each way, and puts the slots and the yard bit back. The three write only inside the
  footprint; the clear nests re-claims through its bucket walk's callback, so the saved frames are
  a stack, two deep in play. A deeper call or a footprint wider than 64 cells either way is counted
  and named. Without it the host's slots differ from the recording's where two footprints
  contest a cell: every puppet seat is type 3, so the host's contest gives the cell to the
  newcomer where the recording machine's incumbent kept it
  ([engine map](../exe-reverse-engineering.md), *The footprint contest's rule*).
  Every recorded footprint cell is the set-position's rounding of the recorded position (MEASURED
  2026-10-12, 3,640,526 of 3,640,526 unit-iterations of two recordings): the placement's stamp is
  not ahead of the recording. A recording plays only when the recorder's hooks
  are in, since the draws, the explosions and this undo are taken there.

**The comparison.** A dump carries the grid, the pool, its heads and the cursor, and
`tarec.py compare` compares them byte for byte less bit 2, live and with `--puppet`.

**Loosened, as the owner agreed on 2026-09-30:** four rows in `replay-layout.tsv` (the cell's draw
bit, the 3DO record's model instance, the GAF record's two sequence pointers as IDs; the second is
none while the record does not use it, when the engine does not read it), and two
things not recorded: the animating defs' idle phases (`FeatureDef +0xCC..+0xE3`, which the tick
steps and only the draw reads [INFERRED]) and the movement classes' passability, which is derived
from the grid and is not refreshed for cells an image writes (puppets do not pathfind).

**Not the stock save's shape.** The save `0x424890` and its load `0x424C00` carry what a new game
needs to look the same, not what makes it continue the same: no record indices, list order,
velocities, frame timers, the shadow sequence's frame, the countdown or the cursor.

#### S3a, verified and measured

MEASURED 2026-09-30 under Wine, format 3, scripted runs on a private X server:

- **After the landing review's fixes** (the layout's hash changed, so every fixture was recorded
  again): the forest fire, the battle, the carry links, a naval battle (`scenarios/naval-push.json`,
  213 deaths, 42,649 wreck records written) and the stock-limits game each recorded and played on
  puppets; every live and puppet dump EQUAL, 0 cells and 0 records left for their structure or
  for a bad value, every footprint write undone. The battle's capture: 564 µs on average, 1.64 ms
  at worst.

- **The fixture**, `scenarios/s3a-forest-fire.json`: six Rocko and two Zeus against six A.K. and
  four Storm in the densest forest of Two Continents, and a construction kbot reclaiming a
  laboratory wreck behind them, recorded for 4,710 iterations and ended by a scenario that clears
  the map. The recording: 4,808 feature roots — 4,710 ticks, 73 weapon impacts' damage outside a
  death and 23 inside the explosions of 7 of the 23 deaths, the reclaim's FeatureDie and the
  scenario's own spawn — and 295 draws (the burn's 76, the spread's 163, reproduction's 56); 0 cells
  changed that no writer had marked. `tarec.py check` explains every change of every step, the five
  snapshots equal the stream, and three dumps taken live are equal to the recording.
- **Played on puppets** (`tacli replay play`, full visibility) on the landing build, seven dumps at
  GameTime 640 to 4,576 are equal to the recording (`tarec.py compare --puppet`): the grid, the
  pool, its heads and the cursor, with the units. Every root was made and returned what it had,
  every draw was taken as recorded, every death's explosion made its calls, no cell or record was
  left for its structure or a value out of bounds, and all 2,295 footprint writes were undone. The
  host's own feature tick was refused 4,709 times and no other feature call was.
- **The first build diverged there**, at the first death whose explosion burnt a tree (GameTime
  1,379): it replayed a death's feature calls by replaying the death, and on the host the owner
  gate makes a puppet's explosion a picture. The explosion's calls are now roots.
- **The cost**, the same run: the capture took 135 µs an iteration on average and 351 µs at most,
  the first 10.5 ms. The stream was 10,209 KB raw and 1,302 KB deflated; 7,077 KB of it is cells,
  almost all the first iteration's whole grid, and 1,260 KB wreck records, a burning feature's
  record changing every tick. The five snapshots were 55 MB raw, each with every cell, and the
  file 4.1 MB.
- **S2b's three games again**, recorded with the features and played on puppets: the 1,432-unit
  battle on Town & Country (3,810 iterations: 5,749 feature roots, 106 draws, 1,432 deaths), the
  carry links (3,090 iterations) and the four-AI game in the stock-limits build (2,010
  iterations, the 2,048-record pool). Every check is clean, no cell changed that no writer had
  marked, and every dump is equal on puppets, five, six and five. The battle's first playback
  found one cell's slot B naming another unit than the recording's, the cause of the footprint
  writers' undo (*Playback*; a contest decided by the puppet's type-3 rule or the stamp's edge
  bound, an aircraft at the map's last columns, not a lead of the stamp [INFERRED, 2026-10-12]); its next found 1,200 of them called inside a clear. With the undo
  stacked, the battle's playback undid 315,067 footprint writes and missed none.
- **The cost in the battle**: 569 µs an iteration on average and 2.21 ms at worst (GameTime
  3,305, 111 KB into the ring), against S2a's 475 µs and 1.15 ms; in the carry-links game 123 and
  324 µs, in the stock-limits game 118 and 642 µs. The battle came to 68.8 MB for two minutes
  (S2a: 60.4): its moving units' footprint cells are 33 MB of its 177 MB stream before deflating.

#### What S3a leaves

- **The capture's cost in a battle** grew further past the product page's budget of 0.5 ms on
  average and 1 ms at worst: 569 µs and 2.21 ms (S2a: 475 µs and 1.15 ms). The pool's comparison
  is 26 µs of it; the rest of the features' share is not broken down.

- **Every snapshot carried every cell**, 7 MB raw on Two Continents and 21 MB on Seven Islands;
  deflated, the fixture's five snapshots came to about 2.8 MB of its 4.1. Since S3b a snapshot
  carries only the cells that differ from the first iteration (*S3b, verified and measured*).
- **Passability** is not refreshed for the cells an image writes (above).
- **A reproducing feature** would replay as recorded, reproduction's row bug included (the engine
  map); stock has none.
- **An indestructible feature that burned** would keep its record active for ever, in the
  recording and in its replay alike; stock has none (the engine map).

### S3b — the globals, and seeking

A recording carries the main block's and `.data`'s simulation fields, the projectiles in flight
and the explored map at every iteration, and a puppet level can be moved to any GameTime of the
recording, forward or backward, in the running playback or from a fresh process
(`tacli replay seek`). The engine facts are
[the engine map](../exe-reverse-engineering.md#the-demo-recorders-globals-projectiles-and-seek-disassembled-measured-2026-10-01)'s.
The owner's decisions behind it (2026-10-01) are in *Decided 2026-10-01* below.

**What it records.**

- **The positive list**, `tagpu/ddraw/replay-globals.tsv`: every byte of the main block and `.data`
  a recording carries, and how — one row a field, with its writers and how they were found.
  `tools/replay-globals-gen.py` generates `inc/tagpu_replay_globals.h` from it (the main block's
  rows as offsets from `main`, `.data`'s as offsets from `0x501000`, so the header names no engine
  address), and `tools/replay-layout-check.sh` refuses a link where the two disagree. A row is
  `globals` (in the globals image) or names the record that already carries the field (`player`,
  `viewer`, `tick`, `features`, `proj`), and a `globals` row says what a puppet host does with it:
  `write` (written at every capture point, at the frame visit and at a seek), `hold` (never written
  on a puppet host, masked in the puppet comparison; recorded for the live restore and the
  takeover), or `react` (LosType bits 0–2, written with the engine's own reaction). The rows: the
  wind `main+0x37EC4..+0x37EE5`, LosType, the game end's countdown `main+0x39239` and flags
  `+0x3923B` (bits 2 and 4 `hold`, the rest `write`), the simulation RNG `0x51FC88`, the meteors,
  the pathfinder's budget table `0x5119E8..0x511A3B`, the 32 movement classes' watermarks
  `0x512374 + 0x20k`, `0x51E53C`, `0x51E684`, `0x51E688` and `0x51E6C4`: a 77-dword image, recorded
  whole at the first iteration (`RK_GLOBALS`) and then as the dwords that changed
  (`RK_GLOBALS_DELTA`), and re-read whole into the rotating check every 30 iterations. The header
  carries the file's SHA-256 (`RT_GLOBALS`), and a recording made with another list does not play.
  **A field the simulation's steps write goes on the list** unless it is the viewer's own or a cache
  of the draw; the TSV's header names what it leaves out and why. Adding a row is ordinary work;
  removing one, or making one `hold`, is a loosening and the owner's.
- **The projectile pool**: the count `main+0x141F3` and the records `*(main+0x141F7)`, `0x6B` bytes
  each, as a fourth record class `proj` in `replay-layout.tsv` whose pointers are IDs — the weapon,
  the target and the attacker units, and the quarry, a record of the same pool. The capture records
  the pool's written extent, which it finds by scanning forward from the last one while a record's
  weapon is not NULL (the slots ever written are exactly those below the count's peak), as
  `RK_PROJ_HEAD` (count and extent), `RK_PROJ` and `RK_PROJ_DELTA`, with a slice in each rotating
  check. A stale pointer keeps its ID, so the recording carries stock's own staleness — the
  compaction's missed re-aims included. The header carries the pool's size (`RT_PROJECTILES`), and
  the replay refuses a recording made with another; a count past it, or a pool that moved, ends the
  recording (`REC_END_PROJ`).
- **The explored map**, `*(main+0x14273)`: whole at the first iteration, in every snapshot and every
  dump (`RK_MAPPED`), and the byte runs that changed in an iteration where it changed
  (`RK_MAPPED_RUNS`), from a comparison of the whole map each iteration. With Mapping off it is
  0xFF from the load and never changes. A map that moves or changes size ends the recording
  (`REC_END_MAPPED`).
- **Format 4.** A snapshot carries only the cells that differ from the first iteration's grid,
  which every reader holds before it reads one, and no table span that is all zero; the globals,
  the pool and the explored map go into every snapshot whole. Sequential reading skips a
  snapshot's payload without inflating it. Each `RT_TABLE` carries a CRC-32 of its declaration's
  layout, so the seek writes a table back only into a host declaration of the same name, stride,
  class, key, flags and layout. Format-3 recordings no longer play.
- **A create carries the water state its unit took from its slot** (`RK_EV_CREATE` is 28 bytes, byte
  24 the low byte of `+0x10A`): the create writes none of it and the engine keeps the value while a
  unit stays in a band where no test fires, so without it a create played after a seek, in a slot
  dead at T, started from the seek host's history (the engine map's *The unit fields playback leaves
  to the host's engine*).
- **A dump** carries the globals, the pool, the explored map, every player's sight counters
  (`RK_SIGHT`) and the main block and `.data` whole (`RK_IMAGE`). Each record is assembled once in a
  buffer that the file and its CRC both read, because the engine's mouse thread writes a few `.data`
  bytes while a dump runs (its tagged lock and the cursor's positions; none recorded), and a body
  past `0xFFFF` bytes is refused and counted rather than truncated. The trigger
  `tagpu_replay.trigger` is now read by the replay's frame visit (below) at most every 100 ms, on
  any level in play: `dump <name>` arms a dump the recorder takes at its next capture point —
  while paused, at the first tick after the pause — and **`dump <name> at <T>`** one taken at the
  capture point of GameTime T exactly, whatever the speed or pauses (up to 8 armed at once; one for
  a GameTime already passed, or a name already armed, is refused with its own line; one a forward
  seek or the level's end passes is dropped with a line). A playing host's speed keys change
  nothing, so stepping a puppet host to T by keys was a timing; the capture point is where every
  dump is taken anyway.

**Playback.**

- **The globals, the pool and the explored map have one writer on a puppet host**: the replay
  writes them (`recorded_put`) at each capture point, first in B2's stub (`tagpu_replay_capture`,
  before the recorder's capture and the unit tick), when the world holds the iteration the host is
  at, and again at the frame visit after a step, once a GameTime, so the draw after step T shows
  what the recording machine drew after its own. In that order: LosType bits 0–2, only when they
  differ: first a step of the replay's own, the temporary sight records dropped through the
  engine's expiry (the engine's LosType commands leave them alone), then the engine's own reaction —
  the bits written, `0x4816A0(1)` when Mapping changed and `0x4816A0(0)` otherwise — then V's sight
  filled again; the units' bookkeeping (below); the declared tables (*S3c-1*); the
  `write` rows; the pool, each ID
  bounded by its table and a record below the count with no weapon refusing the whole write (the
  host's pool is then emptied and the refusal counted); the explored map, V's bit kept set. The
  replay's end-of-level line counts each.
- **The units' bookkeeping** (`books_put`). A unit's water state `+0x10A` and its sight stamp's
  fields `+0x7A`, `+0x7C`, `+0xF8` are left to the host's engine, which derives them from the same
  history as the recording machine — except in the step that makes the unit. The recording machine
  makes a unit where its own code does: in the unit tick, or after the players phase, as Impure's
  scenario applier does at `0x4969D2` and the engine's SHARE give and console fallback do between
  steps (the engine map's *Who makes a unit*). The replay makes every unit of step k in the unit
  tick's stub, before the players phase of step k, whose every-step sight emit and the next
  images' `0x43DB50` then meet that unit one step early. So at every capture point and frame visit
  the replay writes both from the recording where they differ: the water state bounded by its enum,
  the stamp through the engine's removal `0x482090` and stamp `0x482270` (a circle's radius index
  bounded by the frame table's count), so that the sight counters keep describing the stamps the
  fields name. The globals line counts the water states and stamps written and refused, and the
  units written that the replay made in the step before (`made`) and the others (`others`); an
  `other` is noted by name, since it would mean the host's derivation differed somewhere no dump
  looked. The ordering is the invariant: the write runs after the step's last emit and its last
  `0x43DB50`, and nothing between it and the recorder's capture or the next unit tick emits.
- **The feature tick's input is written back** (`features_put`). The cells the replay writes inside
  the feature tick's bracket stay in the host's grid, and a cell the input changed and the rest of
  the step changed back is in no image, so each run written there (x, z and n, bounded by the
  reader) is noted, up to 4,096, and written again from the next iteration's world after its
  image's runs; past the cap the whole grid is written instead. The features line counts `the tick's
  input put back: <n> cells, <o> overflows`. This was S3a's: the carry links' closing scenario
  left two cells naming a dead unit (*S3b, verified and measured*).
- **A built order node's target** goes through the engine's setter `0x489690` at the step the node
  appears. The constructor clears the reference again for an action whose descriptor lacks bit 9,
  and the recording machine's handlers set it afterwards; before, the node stayed empty for one step,
  and at a seek every such target was empty at T.
- **V's energy and metal are 0** on every host. V is record 10, which the player init zeroes
  (`0x46476B`, `0x46477D`) and the level start `0x465E30` does not reach (`0x465F87`); nothing
  changes a type-3 record's pair afterwards. (While V was the lowest unseated record, the level
  start read them from its skirmish settings entry, which the SKIRMISH screen does not fill,
  MEASURED 1365/16385 and 11/10 in three processes.) The resource bar shows 0 either way: it clamps
  to the storages, which the player init zeroes.
- **V's minimap follows V's sight.** The minimap's fog is composited by `0x466C20` only when
  `main+0x142F1` bit 2 asks, and the engine asks for the viewed player's own units' stamps, sets and
  removals; V owns no unit, so under true LOS V's minimap kept the composite of the sight rebuild's
  base, shaded everywhere (MEASURED by M1: 1.4–2.7 % of the colourful terrain in colour while V's
  world view showed everything). V's sight fill now asks and runs the composite, at the first step,
  after every recorded LosType change and at a seek. In a seat's view the seat's own units ask, and
  the engine's 30-tick pass composites it, as in a game played live.
- **A unit's control group** is written through the engine's setter `0x480250`, only for −1 or a
  group below 10, at every step: a raw write of `+0xAC` left the unit in vector 0, where the create
  put it, and its death then left a pointer to the dead unit there.
- **V's sight** sets V's bit of the explored map from `[V+0x146]`, the field the engine's setter
  uses.

**Seeking.**

- **The reader seeks.** `recr_seek(T)` builds an index of the file's blocks at its first call, from
  their headers alone; it refuses a T outside the recording before the world is touched, then
  starts from the last snapshot before T (or the file's start) and reads the stream to T, leaving
  the world holding iteration T with step T's events pending, as a sequential read would.
- **The frame visit**, before the in-play draw call `0x4969CD`, on the game thread between two sim
  steps and on paused frames too: the trigger, then on a level that plays a recording and has played
  its first step a pending seek, or else the frame visit's write. `seek <T>` and `seek <T> dump
  <name>` in the trigger ask for a seek; the play request takes `play <name> [<seat>] [at <T>
  [dump <name>]]`, so a seek in a fresh process is a playing followed by a seek at the first frame
  visit after the first step.
- **The rebuild, `seek_run`**, R0–R17 in `tagpu_replay.c`, whose comment states the invariant: after
  a seek to T the host holds what it would hold at the frame visit after step T−1 of a playing from
  the start, in everything the recording carries, and everything else it keeps is made by the
  creates, reset to what a level start gives, or set from T. R0 positions the reader and bounds the
  world at T — the features' lists and anchors, the buckets the units go into, the pool, the
  tables, LosType — and the host's clock, and refuses the seek on the first failure, touching
  nothing. A host at GameTime 0 (before the level's first step, or after a seek to the recording's
  first iteration that no step has followed) is refused: the expiry `0x482130` removes a temporary
  sight record only while its expiry is below GameTime (an unsigned compare, `0x48215C`), so the
  drop below could remove none of the death vision the kills add. A recorded table is paired only
  with a declaration the recorder records (`tagpu_simreg_recorded`, the one test both sides read:
  `SIM_STATE`, never a lock, code, a guard, a TLS slot or a DLL-held engine pool), so no header can
  name another into the rebuild's writes; a heap table sized otherwise than the recording's is
  counted with the values left. Then the
  rebuild's kills are kept from being the recording's eliminations (a twentieth puppet gate, on
  the skirmish elimination `0x47BD70`, while the rebuild runs); the pool is emptied; every unit is
  killed on the clock the host holds, through C4's destructor wrap with severity and corpse 0; every
  temporary sight record is dropped through the engine's expiry (S3c-3 makes the viewed seat's again
  later in the seek, *The seek's sight records*); the sight is reset with no unit alive
  (`0x4816A0`); GameTime is set to T−1; every unit of iteration T is made in its slot and
  S2b's appliers write it; each unit's own bookkeeping is set from T's image (R8b): the water state,
  the state byte through the setter `0x48B090` as the state events do, and the sight stamp through
  the removal `0x482090` and the stamp `0x482270`, since the seek cut the history the host derives
  them from; the sort buckets are made the recorded ones through the engine's own
  unlink `0x47CB00` and move `0x47CB60`; the features and wrecks are written whole, the host's 3DO
  model instances freed through `tagpu_reclaim` and made again through `0x45A8D0`; the particles,
  explosions, debris and flying pieces are cleared (`tagpu_effects_clear`); the declared tables are
  written back, then each sim file restores what its layouts cannot express
  (`tagpu_patches_restored`, `tagpu_weapons_restored`); the globals, the pool and the explored map
  are written; V's sight is filled and the minimap's fog composited from T's counters (R14); the
  viewed record's radar pass `0x467440` runs once (R14b), so the contact bits are T's rather than
  those the creates left, which hold no contact, and when the host views the recording's viewed
  record bits 8–10 are then taken from the recording (O14, *What S3b leaves*); the deadlines the recording does not carry are reset (V's
  players-phase deadline to the next multiple of 30, every DisplayTimer to GameTime, the notices
  `0x47EE30` and the chat lines `0x463C80`, the level init's own resets) and every seat's pacing
  stamp is set.
- **The lines**: `replay: <name>: at GameTime <T>, sought from GameTime <h> in <ms> ms (read <ms>,
  rebuild <ms>); units <k> killed, <m> made, <r> refused`, then `replay: <name>: the seek to <T>
  wrote: ...` with the rebuild's counts (`units' bookkeeping from T: <w> water states, <s> state
  bytes, <k> sight stamps, <r> refused` among them); a refusal is `replay: <name>: seek to <T>
  refused: <why>`. A seek's dump is armed for T itself, so a second seek before a step drops it
  with its own line rather than taking it at another GameTime under the first seek's name. A playback that has played to its end, or cannot be read on, still seeks: the
  reader positions itself from its index whatever the play state. Only the level's end refuses, and
  the replay logs that itself (`replay: <name> at its level's end, GameTime <t>: the recording had
  ended before it` when it had).
- **`tacli replay seek NAME T [--recording FILE] [--dump DUMPNAME]`**: with `--recording`, a fresh
  process launched as `replay play` launches one, playing from T; without it, the instance's running
  playback moves to T, a finished one included. It waits for the arrival or the refusal line, and
  with `--dump` for the dump's path.
- **`tools/replay/tarec.py`** reads format 4: `check` builds each snapshot from the first
  iteration's grid and its differences, `compare` covers the globals, the pool and the explored map
  (`--puppet` masks the `hold` rows and V's bit of the map), `compare --images DUMP OTHERDUMP`
  reports every differing byte of the two images that no row of the list covers (a report, exit 0),
  and `compare-dumps FILE DUMP OTHERDUMP` compares two puppet hosts' dumps at one GameTime, the
  sight counters included.

#### Decided 2026-10-01

The owner's answers to the plan's questions, as recommended unless said otherwise:

- **The main block and `.data` go whole into every dump, not every snapshot**: nothing reads them
  from a snapshot, since a seek applies only the list, which the stream carries every iteration.
- **The projectile pool is recorded state**, written every step on a puppet host. This supersedes
  S2's decision that the pool is presentation rebuilt from the shots: a seek must show the shots in
  flight at its target, the host's own evolution of the pool is not the recording's (a burst's
  copies draw the simulation RNG), and a takeover can then keep the shots in flight.
- **The game end's countdown and flags bits 2 and 4 are held on puppets** and masked in the puppet
  comparison: either ends the level from the in-play handler.
- **COB script state is not restored at a seek**, accepted for S3b; recording it can be a landing of
  its own.
- **Format 4**; format-3 recordings no longer play.
- **The capture's cost and the files' size grow past the budgets**, accepted, with the numbers
  (V7, V8) put to the owner before the landing.
- **The snapshot interval stays 900 iterations**, and becomes 450 if the worst seek of the
  seek-time series exceeds one second. Replaced 2026-10-10 by the owner's choice of 1,800 at the
  size plan's L4, which relaxes the one-second target for dense battles
  ([making recordings smaller](demo-recorder-size.md#l4-the-measured-hours-the-snapshot-interval-and-r1),
  *Decisions* 12).
- **The explored map is recorded every iteration**, the first of three options: exact at every step
  and every seek, at the cost of one comparison of the map a capture.
- **The temporary sight records are dropped** through the engine's expiry at a seek and at a recorded
  LosType change (O9). Reversed for the seek the same day, before S3c-3 landed: the seek makes the
  viewed seat's again (*S3c-3*, its *Decided 2026-10-01*).
- **CLAUDE.md's recorder sentence names the globals and the pool**, and with them the explored map.
- **What the positive list leaves out is confirmed**; S3c's null twin classifies it.
- **The rebuild's flag is `SIM_TRANSIENT`**, as S2b's `s_open`.
- **The declared tables are not written between seeks** (deferred to S3c); S3c-1 closes it
  (*S3c-1*).
- **A seek lands on any GameTime it is asked for**, and for up to 29 ticks after it, until the
  viewed record's next radar pass, the contact bits in any view but the recording's own viewed
  record — and in every seat's view the minimap's fog — are computed from T's world where a
  playing's are its last pass's (O14, *What S3b leaves*). In the recording's own viewed record the
  seek takes bits 8–10 from the recording, and those are a playing's. The alternative was to round every target down to the viewed record's last pass tick,
  a seek granularity of one second.
- **The COB guard's stop is a landing of its own.** A unit script's CALL that finds no free thread
  record stops the game under the guard published in v0.3.1 (stock lets that script thread wait for
  good); S3b changes nothing for it, and a recording that ends with Impure stopping the game keeps
  having no end block.

#### S3b, verified and measured

MEASURED 2026-10-01 under Wine, scripted runs on a private X server, through the runner's parallel
jobs; each game ten minutes or less. The landing's build is `adaaf366`; its recordings were made on
`f3af9f9e`, whose recording format it keeps (the later commits change no table and no record).

- **Recorded**, ten games on `f3af9f9e` (batch C1): S3a's five fixtures — the forest fire, the naval
  push, the 1,432-unit battle, the carry links and the stock-limits four-AI game (three times) — the
  naval push with Mapping on (LosType 5: the map starts unexplored), the battle under true LOS with
  `LOS` typed once mid-game (GameTime 1959), and the forest fire under the circle (`"los":
  "circular"`). Each recording complete, with 0 corrections and 0 ID rows outside their tables; all
  38 live dumps EQUAL to their recording, globals, pool and explored map included; and `tarec.py
  check` finds every snapshot equal to the stream (2 to 5 a recording) and every change between two
  iterations explained by its events. On the earlier build `fc5978d2` (V2), with Mapping on the
  explored map changed in 226 of 3,810 iterations (5,669 bytes in their runs); with it off, in none.
- **Played on puppets from the start** (V3), the six fixtures on `adaaf366` (batch D2): **45 of 45
  puppet dumps EQUAL** (`tarec.py compare --puppet`), among them two taken at exact GameTimes in each
  fixture, at the first image of the unit its closing scenario makes and one GameTime later. In every
  playing the pool's write was refused 0 times, every footprint write undone, no create refused, no
  unit missing or present on the host alone and no cell or record left for its structure or a
  value out of bounds; the units' bookkeeping wrote only for units the
  replay had made in the step before (206 in the naval push, 20 in the forest, 1,429 in the battle, 6
  in the links, 1 in the stock game), with **0 others** and 0 refused.
- **Seeks** (V4 fresh-process, V5 in one process), on `adaaf366`: a fresh seek in the naval push to
  3174, with exact dumps at 3644 and 3700; in the battle, seeks to 2296, back to 794 twice and to
  2296 again after the playing had reached its end; in the unmapped naval push, to 2408, back to 848
  twice, forward to 3174, and back to 2408 after its end — **every dump EQUAL**, and the two dumps of
  one target in one process identical. The batch before (C2, `f3af9f9e`, every fixture) had 108 of
  109 dumps EQUAL; the one that differed found the units' bookkeeping (below).
- **Sight under true LOS and the circle** (V10, V10c), in V's view: played from the start with dumps
  armed at 901, 910, 931 and 2401; in the same process seeks back to 901, forward to 2401 and back to
  910, then on to 931; and a fresh-process seek to 901. Every dump `--puppet` EQUAL (8 a run), and the
  strict two-host comparisons (`tarec.py compare-dumps`, sight counters and contact bits included)
  EQUAL: the seek at 901 against the playing at 901, at 2401 against 2401, the fresh seek against the
  in-process one, and the dump at 931 after the seek to 910 against the playing's at 931. In a seat's
  view (`--perspective 1`), in-process and fresh-process seeks to 1700 and 2560 compared strictly
  EQUAL, every dump `--puppet` EQUAL; those seeks dropped 1 and 20 temporary sight records.
- **O14's residual, shown** in V's view: the battle under true LOS, dumps armed at 2490 and 2491 and
  in-process seeks to each. At 2490 the window is steps 2461..2489, in which the recording makes unit
  1502 (step 2483): the seek's dump against the playing's differs in exactly that unit's `+0x110`,
  bits 8 and 9 set on the seek host and clear on the playing; at 2491, a pass tick, they are strictly
  EQUAL; all four `--puppet` EQUAL. (The V10 runs' 910 showed no difference because neither
  recording makes a unit in steps 901..909.)
- **What the runs found, each now built** (*Playback*, *Seeking*):
  - **The feature tick's input**, an S3a gap: on `fc5978d2` and `a8d17df2` 32 of 33 puppet dumps were
    EQUAL, and the carry links' sixth (GameTime 2,967) held unit 1501 in cells (121, 491) and (122,
    491): the step to 2945's bracket wrote them when the commander's unit tick moved it a row north,
    the closing scenario's deaths in the same step cleared its footprint, and image 2945 carries rows
    492 and 493 only.
  - **The units' own bookkeeping at a seek** (R8b): every unit of every arrival dump (39 dumps, six
    fixtures) differed in the sight stamp's fields, a few in the state byte or the water state, and
    about 1,800 ticks later the stamp's height byte still differed by 1 to 4 on 1 to 8 units (the
    emitter's hysteresis).
  - **A create in a slot dead at T** started from the seek host's water state: in the naval push
    units 504 and 506 held 0 after a fresh seek where the recording holds 2.
  - **A unit made after the players phase**: after a fresh seek to 3174 in the naval push, the dump
    at 3644 held the closing scenario's unit's stamp (31, 29, 136) where the recording holds the
    create's (0, 0, 0). Played from the start on `87e70a5e` the same difference appeared in the naval
    push (unit 506 at 3644) and the forest (unit 501 at 4539), each EQUAL one GameTime later: the
    playing's, not the seek's.
  - **Built order nodes' targets**: 1 to 3 nodes a dump held an empty target at T in four fixtures.
  - **V's resources and minimap**: V's energy and metal differed between processes (1365/16385 and
    11/10); V's minimap under true LOS showed 1.4 %, 2.4 % and 2.7 % of the colourful terrain in
    colour before the LOS change (M1). With the fix (M1b, `f3af9f9e`): **92.7 % at GameTime 42, 93.5 %
    at 113, 94.5 % at 933 just after a seek to 901, 94.4 % at 1514, 94.5 % at 2317 and 99.1 % at
    3015**, against seat 1's picture at 3021 with every cell in sight (`judge/mm_colour.py`). No
    picture before GameTime 30 could be taken.
  - **Unreadable dumps**: 2 of 16 stock-fixture dumps carried a CRC that disagreed with their bytes
    (2 of 416 saved dumps in all). Each sums right with only the engine mouse thread's 8 lock bytes
    in the lock's other state (the engine map's *mouse thread*); a dump now sums the copy it writes.
- **The COB guard's stop** (O15, not this landing's): in one process the unmapped naval push was
  sought to 2408 and back to 848 twice, every dump EQUAL, and about 740 ticks later, at about
  GameTime 1586, Impure's guard stopped the game (`CORMSHIP.COB`, word 724, `CALL cannot obtain a
  child thread`). A single fresh seek to 848 stops the same way between GameTime 1550 (its dump
  EQUAL) and 1700. A playing from the start and the recording machine pass that point. Before any
  seek, one of four unmapped-naval recordings on `6275c72d` stopped at about GameTime 620
  (`CORMSHIP.COB`, word 405).
- **Seek times** (V6), the battle alone, 10 in-process seeks and 2 fresh-process ones, every dump
  `--puppet` EQUAL and 0 refused: **27–33 ms to a target 1–10 ticks after a snapshot** (read 8–13,
  rebuild 15–20) and **461–605 ms to one 1–10 ticks before the next** (read 446–587): reading forward
  from the snapshot dominates. The worst is 605 ms, under the product page's one second, so the
  snapshot interval stays 900. Wall time 0.4–1.0 s in process, 22.2–22.7 s for a fresh process with
  its launch. Across every seek of the C2 and D2 batches, all fixtures: 5 to 611 ms, the rebuild at
  most 35 ms.
- **The capture's cost** (V7), the battle recorded alone on `adaaf366`: **585 µs an iteration on
  average and 1.44 ms at worst** (GameTime 36), the first iteration 7.85 ms, against S3a's 569 µs and
  2.21 ms. The file was 81.5 MB, the stream 202.4 MB raw and 80.8 MB deflated; the pool's records 25.0
  MB of the stream raw (12.4 %; its written extent reached 209 of 3,000), the globals' 166 KB, the
  explored map 142 KB in 5 records.
- **The sizes** (V8), C1's recordings against S3a's of the same fixtures (a MB is 10⁶ bytes; S3a's
  files read from their block headers):

  | Fixture | Iterations | File, S3a → S3b | Stream raw → deflated, S3a → S3b | One snapshot raw → deflated, S3a → S3b |
  |---|---|---|---|---|
  | forest fire | 4,710 | 4.11 → 1.88 MB | 10.4 → 1.31 MB, 11.5 → 1.59 MB | 11.3 MB → 560 KB, 0.80 MB → 57 KB |
  | naval push | 3,810 | 6.57 → 10.46 MB | 18.0 → 5.03 MB, 25.6 → 10.15 MB | 8.7–8.8 MB → 381–386 KB, 0.75–0.78 MB → 76–80 KB |
  | battle | 3,810 → 3,840 | 73.98 → 91.21 MB | 193.1 → 72.15 MB, 224.9 → 90.53 MB | 8.6–8.7 MB → 440–462 KB, 1.30–1.42 MB → 161–164 KB |
  | carry links | 3,090 | 1.81 → 0.92 MB | 5.5 → 0.71 MB, 5.7 → 0.75 MB | 8.1 MB → 364 KB, 0.70 MB → 55–56 KB |
  | stock limits | 2,010 | 0.92 → 0.45 MB | 6.3 → 0.38 MB, 6.8 → 0.42 MB | 6.3 MB → 265 KB, 0.39 MB → 16–17 KB |
  | naval push, Mapping on | 3,810 | — → 10.58 MB | —, 25.8 → 10.27 MB | —, 0.75–0.78 MB → 74–79 KB |

  Snapshots are 6 to 16 times smaller raw and 3 to 16 times deflated, and the files of the quiet
  fixtures halve. Where shots fly the files grow, the naval push by 3.9 MB and the battle by 17.2 MB,
  from the stream: the pool's records are the largest addition.
- **The images report** (V9), 24 pairs — each fresh-seek dump at T against the recording's live dump
  at T — all exit 0. Outside `replay-globals.tsv`, per pair, 1,154–3,476 bytes of the main block in
  717–1,279 runs and 322–1,919 bytes of `.data` in 160–1,099 runs. Grouped by the tool's coarse map,
  the largest are the debris pool (600 runs), the desktop GUI (121), the weapon defs (513 runs, in 4
  pairs), the GAF sequence pointers of render assets (74), the map's wreck pool, heads, feature grid,
  wind limits, gravity and sea level (26), the movement classes in `.data` (60) and file-backed
  constants and tables (1,216); the mouse thread's lock and the cursor's drawn position each appear
  in one pair. This is data for S3c's null twin to classify, not a list of defects.

#### What S3b leaves

- **COB script state is not restored at a seek**: every unit is made again, so every script
  restarts (the owner accepted it for S3b; recording it can be a landing of its own). Two
  consequences inside that: the seek sets a unit's water state without starting its
  `setSFXoccupy`, so until the unit's next change its script does not know its water state (in
  stock scripts it picks the kind of `emit-sfx`, a wake or not [INFERRED from the scripts'
  convention]); and the threads alive on a unit after a seek are not the recording machine's.
- **The COB guard can stop a playback after a seek** where a playing from the start passes (*S3b,
  verified and measured*: `CORMSHIP.COB`, word 724). No script thread leaks through a seek — the
  destructor deletes each unit's COB object and the create makes one with all eight thread records
  free (the engine map's *A unit's COB object*) — but the restarted scripts can find all eight busy
  where the playing found one free [INFERRED from that one script]. The guard, published in v0.3.1,
  then stops the game where stock would leave that script thread waiting. The remedy is the guard's
  own landing (O15), not the recorder's.
- **For up to 29 ticks after a seek in any view but the recording's own, the contact bits are not
  a playing's** (O14). They are display state, written on a puppet host only by the viewed record's
  30-tick radar pass, which reads the units the unit tick of its step has just moved and the
  globals mid-step, so no image reproduces it (the engine map's *The radar pass `0x467440` and the
  minimap's fog composite*). The seek runs that pass once on T's world (R14b), where a playing
  still holds what its last pass, in step P = 30·⌊(T−1)/30⌋, computed. **When the host views the
  recording's viewed record the seek and the takeover then take bits 8–10 from the recording** (a
  takeover views P from TK1 on; a multiplayer recording's takeover keeps the pass's, the
  multiplayer slice's *The takeover of a multiplayer recording*), on every slot live
  in the image whose host slot holds a live unit of its type: in that view a playing computes the
  same pass from the same inputs at the same phase (D1), so what the recording holds at T is what a
  playing holds — every capture-point dump in that view compares the radar picture strictly and is
  EQUAL. That closes O14 for the contact bits in that view at every T, T = P+1 included, where the
  pass alone is wrong whenever something the pass read changed later in its step: MEASURED
  2026-10-09, the tenseat fixture's LOS command typed at GameTime 900 leaves images 901..930 with
  LosType 6 and the bit 8 a LosType-4 pass set on its 19 enemy units; the pass alone cleared them,
  and the round trip at 901 differed A against B in exactly those 19 `+0x110` (and at 911 in a
  process with no round trip before it); with the recorded bits the round trips at 901, 911 and 931
  are EQUAL. Its residual: a recording whose viewed record changed within the 30 ticks before T
  holds the old record's scan until its next pass, where a playing holds its own. In any other view
  nothing recorded is the truth, and the seek shows the pass's contacts at T until the next pass:
  in V's view only `+0x110` bits 8 and 9 of a unit the recording makes in steps P+1..T−1 (set after
  the seek, clear on the playing, unless the unit's def sets bit 9) and bit 12 where such a unit's
  slot held it (clear after the seek, kept on the playing) — MEASURED once (*S3b, verified and
  measured*, unit 1502 at 2490); in another seat's view also the units that entered or left that
  seat's radar since P+1, established by the disassembly, not measured. In a seat's view, the
  recording's viewed record included, the minimap's fog R14 composites from T's counters is T's
  where a playing shows its last pass's. From the next pass on, scheduled at the same GameTime on
  both hosts, the two agree. **In a takeover the bits are also the fork's AI input** (the owner's
  decision, 2026-10-11: the fork's AIs read the recording's contacts at T): the acquisition scan
  `0x40AA40` puts a non-allied unit with bit 8 on every local seat's radar list
  (`0x40AB3F..0x40AB5C`), and each acquisition record TK1 makes starts with `+0xED` 0 (`0x40918B`,
  `0x4092DC`), so it scans in iteration T's players phase (`0x40B2D6..0x40B2E5`), before the next
  radar pass, and reads the bits R14b left. Nothing after R14b writes bits 8–10 before the capture
  point of T (R14c; TK4a's `0x48BD00` clears bits 4, 6 and 7 only and `0x466DC0` reads; the tick
  loop to `0x4954ED`), so in the recording's viewed record the fork's first iteration holds the
  recording's contacts. As another player the pass's bits stand, and they are the AIs' input until
  P's next pass; D2 (d) masks bits 8–10 there, and bit 12 always. MEASURED 2026-10-11 on tenseat
  (`tareplay.py takeover`, release build, game speed 20, turbo 1), compared by `tarec.py
  compare-fork`, which compares bits 8–10 strictly because both worlds view player 0:

    | T | the takeover | compare-fork |
    |---|---|---|
    | 901 | with the restore | EQUAL, 23 live units; F's dump f1201, timeline 1 played across its boundary, EQUAL, the radar picture too |
    | 915 | with the restore | EQUAL, 25 live units |
    | 915 | without it (the pass's bits) | 21 differences in 22 live units, every one `+0x110` bit 8 |
    | 885 | either | EQUAL: nothing the pass reads changed in 871..885 |

    `tarec.py` prints a field's bytes in memory order, so the difference `21212830` against
    `21202830` is the dword `0x30282121` against `0x30282021`: bit 8, not bit 16.

- **A unit the recording machine made after its players phase is drawn one frame early** on a puppet
  host, which makes it in the step's unit tick, before that step's draw; `books_put` makes its
  bookkeeping the recording's at the next capture point, and the picture of one frame stays. Its
  first `setSFXoccupy` also starts twice there, at the step's images and again at the next, once the
  water state is put back. Both are presentation on a puppet host. Whether a stock skirmish's SHARE
  dialog lists an AI ally, the one stock way to make such a unit without a cheat, is not established
  (the code accepts any active type-1–3 record without the watcher bit; not run); `books_put`
  covers it either way, since it keys on the replay's create, not on its cause.
- **Two dumps of one GameTime can differ in the engine mouse thread's bytes**: its tagged lock, the
  lock's owner tag and the cursor's two positions, which the recording does not carry. How long the
  thread holds its lock, and what Wine's own DirectPlay and DirectSound threads write, are not
  established.
- **The temporary sight records at a seek — closed by S3c-3**, which makes the viewed seat's again
  from the recorded deaths: each death event already carries the victim's image as the destructor
  found it, so no recorder change was needed. **At a recorded LosType change they are still
  dropped**: up to 2 s of the viewed seat's death vision, under true LOS and a seat's view only.
- **The declared tables between seeks — closed by S3c-1.** In S3b a puppet host wrote them only at
  a seek, and the wind's at the first step, so between seeks each held what the host's own code
  wrote; `s_quiet`, written under the gated damage apply, made the minimap's blink after harmless
  weather differ during playback. S3c-1 writes every recorded table at every capture point and
  frame visit (*S3c-1*).
- **The AI, the pathfinder and the objective objects** (heap, outside both images) and the CRT's
  `rand` (per thread) are not recorded; a puppet host runs no AI and asks its pathfinder for
  nothing.
- **Passability is not rebuilt at a seek**, as it is not refreshed for the cells an image writes;
  a takeover must rebuild it.
- **Host-own state survives a seek**, none of it read on a puppet host: the passability cache and
  the order of the units inside each seat's control-group vectors (their membership is the
  recording's). The takeover (S4) must answer for each.
- **The rebuild runs on a puppet host only**; the live in-place restore comes with S4's takeover
  (the owner's answer to OQ1, *S3c*).
- **The images report is informational.** `tarec compare --images` names the bytes outside the
  list (*S3b, verified and measured*, V9), but nothing yet sorts them into simulation and
  presentation; S3c-2's null twin does, and makes it a gate.
- **The ray-fan edge**: a unit whose sight height is 5 or less is not re-stamped after its height
  byte is zeroed but is still unstamped at its death (the engine map); whether any unit type
  reaches it is not established.
- **The capture's cost and the files' size grew further past the product page's budgets** (0.5 ms
  on average and 1 ms at worst): 585 µs and 1.44 ms in the battle, and the battle's file 17 MB
  larger, the pool's records the largest addition to the stream.
- **Not measured**: the playback's own cost per step, and the cost of `0x4816A0` over a whole map.

### S3c — the round trip and the harness

Planned 2026-10-01, once S3b was in, from the
[completeness note](demo-recorder-completeness.md#the-replay-test-harness)'s harness and the owner's
answers of that day (its *Decisions taken*). Three landings, each verified by running it:

| Landing | Delivers |
|---|---|
| **S3c-1 — one writer for the tables, and the round trip** | on a puppet host the recording writes every declared table at every tick boundary; frame dumps; the puppet round trip with the TRANSIENT reset and the poison check; `tools/replay/tareplay.py run` and nine fixtures in the tree |
| **S3c-2 — counted on puppets, and the images gate** | root, sink and COB-write counts in every dump and their comparison; the null twin and the images gate over the bytes `replay-globals.tsv` leaves out; the address-shaped-value scan |
| **S3c-3 — fixtures beside the code, and the gate** | `SIM_FEATURE` regions naming their fixtures, the coverage build, `tareplay.py landing` and `broad`, the CLAUDE.md replay gate and the landing command's Step 4b |

**Why the round trip runs on a puppet host, not on the live skirmish** (the owner's answer to OQ1).
The recording machine cannot rebuild the state it is in. The recorder captures iteration k at the
capture point inside step k, after GameTime's increment and before the unit tick (the engine map's
[*tick boundaries*](../exe-reverse-engineering.md#the-demo-recorders-tick-boundaries-the-sim-step-0x495490-and-its-callers-disassembled-2026-10-01)),
so at the frame visit after step k it holds iteration k while the world holds step k's result: a
rebuild there restores iteration k and step k runs a second time, live. That second run meets what
no recording carries — each seat's AI object and acquisition record (a simulation-RNG draw at
`0x40B301`), the per-thread CRT `rand`, and every live order node's handler object `+0x52`, which
cannot be copied (MEASURED 2026-09-29: a fault at `0x43A2A2`; the engine map and the
[state evidence](demo-recorder-state-evidence.md)). Made safe, a live
rebuild needs the recording ended first, the elimination refused, the objective's end tests held,
the order nodes' handler objects adopted and the `hold` rows written: that is S4's takeover, which
brings the live in-place restore with it. A puppet host's frame visit after step k already holds
iteration k+1 (`play` writes its images in step k), so its rebuild to k+1 is in place, and the
whole-image comparison the live round trip was for comes from the fresh-process seek of S3c-2's
images gate.

### S3c-1 — one writer for the tables, and the round trip

**The declared tables have one writer on a puppet host** (O13, closed by construction). The replay
writes every table the recording carries into the declaration it pairs with (`tables_put`,
`tagpu_replay.c`) from `recorded_put`, after the units' bookkeeping and before the globals: at each
capture point (`tagpu_replay_capture`, first in B2's stub) and at each frame visit after a step. R12
of a seek calls the same code, and R13's `recorded_put` writes the tables once more. The invariant,
stated at `tables_put`: on a puppet host the recording is the only writer of every table it carries
at every tick boundary — at each capture point and each frame visit after a step, every recorded
table this build pairs holds the recording's values for the iteration the host holds, less the
fields its layout keeps. Host writers in between (a replayed death's credit, a create's birth stamp)
are overwritten at the next boundary. It is an ordering: `recorded_put` runs on the game thread,
in B2's stub before the unit tick and in the frame visit before the draw.

- **Only the bytes that differ are written.** A row's bytes outside its layout's keep (`table_keep`:
  `SIMF_LOCAL`, `SIMF_OWNED`, `SIMF_COLUMN`, `SIMF_ADDR`, and a `SIMF_COUNT` in a row with column
  fields) are compared and written where they differ; a `SIMF_ADDR` with an ID kind is written back
  as the address its ID resolves to, and counted as kept when it does not. A count variable is
  kept whole when the table it counts is a heap table, or a live prefix whose recorded count exceeds
  the array (`count_kept`). The write holds the air pool's lock from `tagpu_patches_restore_begin`
  to the end of `tagpu_patches_restored`, then `tagpu_weapons_restored` re-establishes what the
  weapon defs' layout cannot say, every call. A table the recording carries but this build does not
  pair is named once a playback (and at every seek).
- **The globals line names the tables the host wrote.** Its tail is `the declared tables: <r> rows
  written over the host's own values (in <names>), <k> values kept, <s> skipped, <p> repaired`,
  the names as many as fit in 160 characters. A row counts when the write changed any of its bytes,
  so `<r>` counts the host's own writes the recording overwrote.

**Frame dumps.** `dump <name> now` in the trigger takes a dump at once, at the frame visit that
reads it, on any level in play (`tagpu_rec_dump_now`; refused with a line when no level is in play,
the name 1 to 39 letters, digits, `-` and `_`). Its header carries `RT_FRAME`, the GameTime at the
dump, and its records are read as iteration GameTime+1: between two steps they hold step GameTime's
result. It is not that iteration's capture point: its main block and .data images also hold what the
rest of the frame wrote — the draw, the next frame's scheduler `0x495230` and its pacing words — and
GameTime is one lower (the engine map's *tick boundaries*), so a frame dump compares only with
another frame dump; the round trip's A and B are taken at one frame visit. Two frame dumps with one `RT_FRAME` saw no step between them:
GameTime rises once a step, at `0x4954C0`. The log line is `recorder: frame dump <name> at GameTime
<g> (iteration <g+1>) -> <path> ...`. `tools/replay/tarec.py` reads `RT_FRAME`: `compare-dumps`
labels the pair `frame dump at GameTime <g> (iteration <g+1>)` and refuses a frame dump against a
capture-point dump or one of another frame; `compare` refuses a frame dump (`compare it with
compare-dumps`), since no capture point's image holds what it holds.

**The round trip.** `roundtrip <name> at <T>` in the trigger, on a level that plays a recording
(refused elsewhere), T ≥ 2 and the name 1 to 36 letters, digits, `-` and `_`, up to 24 armed. At the
first frame visit of GameTime T−1, the reader at iteration T, right after that visit's
`recorded_put` — so A's tables are the recording's, which the poison check needs — the replay takes
the frame dump `<name>-a`, puts the TRANSIENT declarations back, seeks to T with the poison check,
takes the frame dump `<name>-b` and logs one line: `replay: <rec>: round trip <name> at <T>:
TRANSIENT <b> bytes reset (<names>); <n> tables poisoned, residue <r> (<names>); seek <ms> ms`.
`tarec.py compare-dumps <rec> <name>-a <name>-b` must be EQUAL: A is a playing's state, B a rebuild
to T in place with nothing between them, so a difference is the seek's by construction. A round
trip already behind the reader is dropped with `... not run: the level is at
GameTime <t>`, and one whose seek is refused logs the refusal and takes no B.
For an upcoming target, the byte-matched observer at `0x49692D` bounds the due-tick
batch before `0x495490` caches it in EBX. The batch cannot advance beyond T−1,
so ordinary simulation progression reaches that frame at any supported speed.
The observer changes pacing only on a puppet with round trips armed, on the same
game thread that arms and runs them. A round trip is refused if this guard could
not install. Explicit seeks can still move the reader past an armed target.

- **The TRANSIENT reset** (`tagpu_simreg_transient_save`, `tagpu_simreg_transient_reset`,
  `tagpu_simreg.c`): the static `SIM_TRANSIENT` declarations — not TLS, heap or engine ones —
  `stride × nrows` bytes each, saved at the playing's first frame visit after its first step (a
  tick boundary) and put back only at a round trip's start, which names up to 8 that differed
  (`<name>: <k> bytes`). At a tick boundary every byte of one is at its value from load or written
  before it is read (`tagpu_simreg.h`), so the reset changes nothing the simulation reads unless a
  declaration is misclassified, which tareplay's continuation dump 300 ticks later names.
- **The poison check, as built** (R12, when the round trip asks). Every declaration
  `tagpu_simreg_recorded` accepts — not only those the recording pairs — is copied aside and every
  byte its layout does not keep is set to `0xA5` in every host row; then the write; then the
  residue test. The test covers only the bytes the write must cover: outside the layout's keep
  (an address field is never poisoned, since a byte of a host address can be `0xA5`), in the rows
  the write covers (a live prefix's rows below its count, no more rows than the recording carries),
  of a declaration whose value the write does not keep whole (`count_kept`: a heap table's count).
  A byte there still `0xA5` where the recording's own byte is not (for a table the recording does
  not carry, where the host's was not) is residue: each run named (`round trip <T>: <table> row <r>
  +0x<off>: <n> bytes not written by the seek`, the first 16), counted and put back from the copy.
  Every other poisoned byte is put back silently. The invariant is an ordering: copy, fill, write
  and check run back to back inside R12 under the air pool's lock, on the game thread, with no
  engine or simulation code between them, so no `0xA5` outlives R12; poisoning before R1 would let
  the kills' and creates' hooks read `0xA5` from their own tables. A byte the recording itself
  holds as `0xA5` is never residue, so in a table the recording carries the test has no false hit.
  A count that sizes a heap table (`g_side_rows`) is never poisoned: `table_put` bounds that
  table's rows by it and keeps it whole, so a poisoned one would make the bound `0xA5A5A5A5` × the
  stride and, with a recording image larger than the host's block, the write would run past it.
  The copy and every declaration's keep are allocated before the first byte is poisoned, so no
  failure path is left once it has begun.

**`tools/replay/tareplay.py`.** `run FIXTURE... [-j N] [--dll PATH] [--stock-dll PATH] [--keep]
[--out DIR]` (`all` for every fixture) and `list`. For each fixture four GameTimes, each T ≡ 1 (mod
30), the radar pass's step, where a seek's contact bits in any view but the recording's own, which
its own radar pass makes, agree at once unless something the pass read changed later in its step
(O14) — a seek in the recording's viewed player, which the R and Q processes play in, and a
takeover as that player take them from the recording and agree at every T — in
[301, min(0.8·end, end−330)] — room for the
continuation dump 300 ticks after the last round trip — drawn by a generator seeded with the
fixture's name and HEAD and printed: T1 the smallest, T2..T4 the round trips, Tlast = T4. The lower
bound is 301, not the plan's 90, and there are no dumps at GameTime 30: the dumps are armed through
the trigger after the launch returns, so the first must fall past the launch [the margin of 300
ticks is a choice, not a measurement]. Four processes, each
a game of its own:

| Process | What | Dumps |
|---|---|---|
| **L** live | the fixture recorded | `l<T1>`, `l<Tlast>` |
| **P** playing | `replay play` from the start, in the recording's viewed player's perspective | `p<T1>`, `p<Tlast>` |
| **R** round trips | the same, with `roundtrip rt<T> at <T>` for T2..T4 | `r<T1>`; `rt<T>-a`, `rt<T>-b`; `rt<T>c` at T+300 |
| **Q** fresh seek | `replay seek <T1> --recording` in a fresh process | `q<T1>` |

Every check fails by name: the recording's end block (complete, 0 ID rows outside their tables, 0
nano sprays uncredited) and `tarec.py check` (which also fails on any correction in the stream); L's
dumps `compare`; P's, R's and Q's capture-point dumps `compare --puppet`; each round trip's residue
0 with at least one table poisoned, and `compare-dumps rt<T>-a rt<T>-b` EQUAL, strictly: every
player's sight counters and the units' contact bits included. A COB stop in a process's log fails that process. Exit 0 when every check
passed, 1 on a named failure, 2 when something could not run and nothing failed: a process that did
not run (any error, a tool's timeout, a round trip the host passed, a live game that misses its
deadline of 120 s + 4 × its end's real time) is reported as could not run and its checks are not
made, and a missing or unreadable dump is could not run, never a failure. Each worker owns one
tacli instance, `tareplay-<k>`, on its own Xvfb in `:300..:399`, a number taken only once the
server's `-displayfd` pipe confirms it and skipping any with a socket or a lock, and holds an
exclusive lock on `tagpu/instances/tareplay-<k>.lock` for the run, so two runs take different k and
never touch each other's games; nothing is stopped by name or pattern. SIGTERM or Ctrl-C stops the
run: no new game starts, each wait gives up, and every worker stops its game and its Xvfb. Passing dumps are deleted unless `--keep`; the run's folder is outside
the repository (`--out`, `$TAREPLAY_OUT`), since its logs carry absolute paths.

**The fixtures**, `tools/replay/fixtures.json`, keyed by GameTime: `load`, a scenario under
`scenarios/`; `steps`, each `[GameTime, kind, argument]` — `apply` a scenario, `type` a chat line,
`order` one unit by roster type and rank; `end`, the GameTime at which `scenarios/replay-end.json`
ends the level (every unit gone, one AI unit kept, so the recording closes complete); `los: "file"`
to keep the scenario's own line of sight; `dll: "stock"` for the stock-limits build; and `content`,
a command run before every launch of the fixture with the files it writes in the game folder,
which every other fixture's launch removes first. Nine fixtures: `forest-fire`, `forest-circle`,
`naval-push`, `naval-unmapped`, `battle`, `battle-los` (`+los` typed at 1900), `carry-links` (two
transports unloaded at 1050 and one loaded again at 1950), `stock-limits` (a four-AI game on the
stock-limits build) and `nomapalert` (its `content` writes `zzkeys.ufo` through
`tools/datakeys_fixture.py`).

#### S3c-1, verified and measured

MEASURED 2026-10-01 under Wine, scripted runs on private X servers; each game ten minutes or less.

- **The nine fixtures** (`tareplay run all -j 4 --keep`, at f463c10d, 1,200 s): 151 of 153 checks
  pass. The two failures were battle-los's round trips at 1711 and 1891: A's viewed record's sight
  counters one higher than B's in 362 and 213 cells, and at 1711 eight units' `+0x110` bit 8 set in
  A (and in the recording at 1711) and clear in B. A held 2 and 1 temporary sight records, B none;
  at 3031 both held none and the round trip was EQUAL. The seek dropped the records and its radar
  pass computed the contacts from the counters without them: S3c-3's seek makes them again
  (*The seek's sight records*). Every other
  round trip: residue 0, seeks 7–601 ms (the battle's 239–601), `s_foot` 9–291 bytes reset;
  stock-limits poisons 21 tables (its build declares fewer), every other fixture 24.
- **O13, shown and closed.** The nomapalert recording played on main's DLL (88996cf6, before
  S3c-1): `compare --puppet` at 421 and 1771 differ in `src/tagpu_datakeys.c:s_quiet` alone, 7 and 8
  dwords, every one 0 on the puppet where the recording holds `0xF0` bytes. On S3c-1 both are EQUAL,
  and played to its end the playing's globals line reads `the declared tables: 20 rows written over
  the host's own values (in src/tagpu_datakeys.c:s_quiet), 5154 values kept`.
- **The poison check fires.** A scratch build whose seek skips `s_quiet` in `tables_put` (never
  committed), `tareplay run nomapalert`: each round trip's line reads `24 tables poisoned, residue
  15001 (src/tagpu_datakeys.c:s_quiet)` with `round trip 481: src/tagpu_datakeys.c:s_quiet row 0
  +0x0: 15001 bytes not written by the seek`, and the run fails those three checks by name.
- **The per-tick table write's cost**, the battle's recording played to its end alone on a timing
  build, 7,974–7,976 calls (two a tick): **mean 2,320 µs, worst 7,231 µs** writing byte by byte;
  1,407 and 4,889 µs by runs of the bytes a layout does not keep; **575 and 3,816 µs** with a table
  that keeps nothing compared in blocks of about 512 bytes of whole rows, and a table's address
  fields listed once rather than per row. Per table before the blocks: `g_def` 362 µs, `s_txKillMark`
  265, `s_txMark` 260, `s_airIn` 210, `s_ghost` 147, `s_quiet` 52, `s_cobRejected` 30, the repair
  hooks 24, every other table under 6. `g_def` and `s_ghost`, whose rows carry address fields, are
  the bulk of what is left.
- **The nine fixtures on the final code** (`tareplay run all -j 4`, at 63e1d501 with the run-wise
  write): 8 of 9 pass every check, 23 tables poisoned (24 less `g_side_rows`), residue 0 in all 26
  round trips that ran, seeks 8–460 ms. The ninth, naval-unmapped, round trips at 751 and 781:
  both pairs EQUAL and both continuations `--puppet` EQUAL (checked by hand from the kept dumps),
  then the R process stopped at about GameTime 1550 on Impure's COB guard (`CORMSHIP.COB`, word
  724, `CALL cannot obtain a child thread`) — **O15**, the stop S3b already met after seeks near
  848 in the same recording, which the owner gave a landing of its own. V1's naval-unmapped, round
  trips at 841, 1411 and 1741, passed it. Then, on the block-wise write (f5fdda42), `tareplay run battle-los carry-links
  stock-limits nomapalert -j 4`: exit 0, 68 of 68 checks, residue 0 in all 12 round trips, seeks
  7–260 ms.
- **The round trip on two S3b recordings**, the naval push (601, 1201, 1801, 2401) and the battle
  (601, 1501, 2401, 3301): **8 of 8 `compare-dumps` EQUAL** and residue 0, seeks 26–412 ms (the
  naval push 26–152, the battle 296–412). The only TRANSIENT bytes reset were `s_foot`'s, 109–308.
- **The poison check's first cut** tested every poisoned byte, and named residue of 143,347 to
  143,367 bytes in every round trip of both recordings: `s_airPool` (up to 131,072 bytes) and
  `s_ghostDone` (12,288), rows past their live counts; `s_hit` row 0 `+0xE`, a byte of its `freed`
  column's pointer; one byte of `g_def`'s type address in two rows (21 and 277 in the naval push,
  19 and 221 in the battle); and `g_side_rows`, the heap table `g_side`'s count. Each was the check's flaw, not the seek's — bytes the write keeps by
  design — and the check above tests only what the write must cover.
- **`tareplay.py run naval-push carry-links nomapalert -j 3`**: exit 0, every check passes (51 of
  51), about 7 min wall. Round trips at 751, 1381, 2311 (naval push), 1141, 1501, 1921 (carry links)
  and 1171, 1651, 1771 (nomapalert): residue 0, seeks 20–163 ms, `s_foot` 25–218 bytes reset, and
  every continuation dump 300 ticks later `--puppet` EQUAL.
- **A fixture's content reaches every other fixture's game** unless removed: before `content`
  named its files, nomapalert's `zzkeys.ufo` stayed in the game folder, and the next fixture's
  playings refused (`the unit types differ from the recording's (325 here, 279 there)`).

#### What S3c-1 leaves

- **S3c-2**: root, sink and COB-write counts carried in dumps and compared (`compare-counts`), the
  null twin and the images gate, the address-shaped-value scan; tareplay adds them to its run.
- **S3c-3**: `SIM_FEATURE` regions and their fixtures, the coverage build, `tareplay.py landing`
  and `broad`, the `replay-roundtrip:` note, CLAUDE.md's replay gate and Step 4b. Until then nothing
  runs the harness at a landing.
- **`s_foot` (`tagpu_puppet.c`) is reset by every round trip** (9–308 bytes). It is the footprint
  writers' undo stack: depth 0 at every tick boundary, and each frame filled when it is pushed, so
  `SIM_TRANSIENT` is right and the reset changes nothing the simulation reads.
- **The round trip runs on a puppet host only**; the live in-place restore and the takeover run
  are S4's.
- **O15 fails a tareplay run** whose naval-unmapped round trips fall near GameTime 750–850 (the
  GameTimes are seeded by HEAD): the R process meets the COB guard's stop about 750 ticks later.
  Until O15's own landing the harness cannot be a landing gate on that fixture.
- **A round trip whose A holds temporary sight records — closed by S3c-3**: the seek makes them
  again, and every round trip compares the sight counters and the contact bits strictly.
- **The playing's globals line** (and its declared-tables tail) is logged only at the recording's
  or the level's end; tareplay stops its games before either, so a run's logs do not carry it.
- **A seek writes the declared tables twice** (R12, then R13's `recorded_put`, which writes only the
  bytes that differ from the recording's after R12's repairs).

### S3c-2 — counted on puppets, and the images gate

What the harness compares beyond the recorded state: what the engine *did* on each machine,
counted where effects leave the simulation, and every byte of the two images the positive list
leaves out. The engine facts are the engine map's
[*counts and the step's phases*](../exe-reverse-engineering.md#the-demo-recorders-counts-and-the-steps-phases-sinks-impacts-emitters-and-the-images-gates-map-disassembled-measured-2026-10-01);
the checks as asserted, the completeness note's
[*replay test harness*](demo-recorder-completeness.md#the-replay-test-harness).

**The counts** (`tagpu_recorder.c`, *THE COUNTS*). One open-addressed table of 4,096 keys
(site, caller, aux) → count, with an overflow counter; script names in a second table of 512. It
counts on a level that records or plays a recording, on that level's game thread only (its thread
ID taken at the first capture point, where the table is reset), never while a seek rebuilds
(`tagpu_puppet_rebuilding`). A caller inside Impure's DLL is its offset with the top bit set, so it
does not depend on where the DLL loaded. Every dump carries the table (`RK_COUNTS`, at most 1,024
rows a record, with the overflow, a bit per sink whose observer is in, and whether the level
counted) and the names (`RK_COUNT_NAME`).

- **Roots**: an increment at the top of the existing observers, before the recording test — the
  create's exit, the destructor (only for a slot the destructor's own alive test passes, the slot
  bounded first), the build done, the state setter (only a call that changes the byte, by the
  setter's own test, since a replayed root's nested call and its replayed event both reach it and
  the second returns early), the two replies, the two nano emitters and the seven feature
  functions. They chain after the puppet gates, so on a puppet they count only what the gates let
  through. The broadcast starts, the start sites, `QueryNanoPiece`, the order dispatch and the
  footprint writers count too, for S3c-3's UNCOVERED; nothing compares them.
- **Sinks**: 21 count-only observers (the [hook map](../gpu-status.md#26e-the-demo-recorder-tagpu_recorderc-tagpu_recwriterc-a-skirmish-on-request)):
  the explosion add, the COB thread allocation keyed by the script's name (read only through the
  engine's own bound on the index), the four sound entries, the projectile init, the three shot
  paths, the impact `0x499EB0` and ten particle emitters. The plan's sink on the particle pool's
  allocation `0x470EB0` became the emitters themselves: its caller is the emitter, not the effect,
  and each emitter makes exactly one allocation a call while the console's SFX byte is clear, so
  the emitters keyed by *their* callers account for the pool call for call. A sound entry reached
  through `0x47F1A0`'s unconditional call of `0x47F0C0` is counted once.
- **COB's engine writes on a puppet** (`tagpu_puppet.c`, *THE COB WRITES ON A PUPPET*): three
  observers count each call from one of COB's five write sites (SET's ACTIVATION, ARMORED and
  YARD_OPEN, ATTACH, DROP) on a puppet's unit outside the replay's own calls, the first 16 logged
  (`puppet: COB engine write on a puppet: <site> on unit <slot> (owner <p>) at GameTime <t>`). The
  gates make them zero by construction; the observers prove it. Their counts go into the dumps'
  `RK_COUNTS`.

**`tarec.py`** gains `counts FILE DUMP` (a dump's counts by site), `compare-counts FILE L_A L_B P_A
P_B [--absent TSV]`, `images-gate FILE L_T1 L_TLAST TWIN1 TWIN2 P_T1 P_TLAST R_T1 Q_T1` and `scan
FILE DUMP`. `compare-counts` refuses unequal windows (both dumps of a pair are capture-point dumps,
so both windows hold the steps [T1, Tlast)), compares roots per site exactly and sinks per key
within `tagpu/ddraw/replay-absent.tsv`'s band (5 % of the larger or 2 calls) less its rows, and
fails on any COB write in the playing's last dump; a difference reads `sink 0x4B08C0 from
0x4B0C4D "QueryNanoPiece": live 273, puppet 0`. The file held 32 rows when the owner decided them
on 2026-10-01: five `same` classes (the three shot builders, the meteor stone's init, Killed),
eight owner-only queries `absent`, the refused script index both ways, the console key's click,
the flying pieces' explosions, smoke and fire both ways, and COB `EMIT_SFX`'s five emitters both
ways. Killed's `same` class and `QueryNanoPiece`'s `absent` row have gone since, each made exact
([the script slice](#the-script-slice-format-19)).

**The step's phases** (the lost impacts, A7). The playing made every recorded feature root at the
replay step, after the host's unit tick and before its projectile tick; a tree the recording
machine's impact felled or its feature tick burned later in the same step was gone before the
host's projectiles reached it, and those projectiles flew on (MEASURED below). Now the recorder
stamps each root with its phase (`RK_EV_FEATURE`'s second byte, `REC_PHASE_UNITS`, `_PROJ`,
`_LATE`; the writer and the reader refuse a value above `_LATE`), set by stubs on the step's calls
of the projectile tick (`0x495513`) and of the explosion tick (`0x495539`) and reset at each
capture. A playing defers a later phase's roots into a store (8,192 events, 2 MB; the bracket
copied, since the reader moves on) and makes them once the host's own call of that phase has
returned, each root followed by the events its bracket holds, in recorded order (`tagpu_replay_phase`, *THE PHASES*); the step's feature images follow the
late phase, since they are the world after every root of the step. Roots inside a death's bracket
go with the death. A step whose phases did not run is finished, in order, at the next replay step
or seek; past the store's room the held roots and the step's remaining ones go in at the replay
step, in recorded order, and the step's feature images with them; an iteration played late goes
in whole. **The cells' unit slots** of the next image go in at the
replay step, ahead of the deferred features: the host's projectile tick tests the units a cell
names, which the recording machine's unit tick had just moved. A slot goes in only when it names a
unit alive on the host, as the feature write takes it, and it stays in a cell whose structure the
feature write finds different and leaves alone. The feature tick, a late root, reads the slots
too — its reproduction tests the source cell's unit slot (`0x424189`) — and the recording machine's
tick saw the iteration's, so each early slot's previous bytes are kept and put back just before
the tick's root, whose input then goes in over them.

**The images gate and the null twin.** `replay-globals.tsv`'s header prose becomes `leave` rows
(carried `leave`, puppet `-`; `replay-globals-gen.py` accepts them and generates nothing), with
two labels corrected — `0x511A40..` is the network statistics, `0x51E710` the music's combat ring —
and new rows from the first run and the scouts: process state (the C runtime's small-block heap
and count, the engine allocator's statistics, the flying pieces' arena, the particle pool, the
shadow encoder's scratch, the libraries' and the C runtime's regions to the end of `.data`), every
wall clock the engine stores, the viewer's own and the draw's (the music, the hot units, the
resource and unit-info memos, the remembered camera position, the desktop GUI's window state, the
cursor state) and the message log `main+0x12EF` with its head and tail. One `write` row is new:
`main+0x14353`, the unit tick's count of the units it ticked. The twin and the gate are tareplay's
(below); a fresh process's seek is the gate's "seek" side, and the round-trip process's first dump
its second playing.

**The scan.** A dump carries `RT_DLL` and `RT_FIELDS`. Impure's tables are tested against every
range, the DLL's included, less the fields their layout says are not plain values; the engine's
records and the globals image against the engine's own ranges only (the owner's OQ10). The first
run's two failures were random words — `s_windState`, the wind's splitmix state, in battle-los, and
the simulation RNG `rngseed` in forest-circle — and its 261 record hits were u16 pairs, none an
address (the engine map). `s_windState` is declared `SIMF_OPAQUE`, a new kind: a plain value whose
bits are arbitrary, recorded and compared as any other and passed over by the scan. `rngseed` is
the same in `replay-globals.tsv`: its row carries `opaque` in the optional tenth column, the
globals image's counterpart (`tools/replay-globals-gen.py`), so the scan passes over its dword
when the dump was made with this checkout's list and scans the whole image otherwise.

**tareplay** (`tools/replay/tareplay.py`). The live process takes the **null twin** once GameTime
passes Tlast + 5: the pause key, frame dump `ln1`, the arrow keys a second each way, a wheel notch
in and out at (512, 384), the cursor at (700, 500) and (60, 300), the in-game menu by Tab
(`ARMOPT.GUI`), frame dump `ln2` with the menu open, then its OK; the twin cannot run when the game
does not pause, GameTime moves between the dumps, or the menu does not open. The checks added: the
counts, L against P over [T1, Tlast); the images gate at T1 (with P, R and Q all run); the scan of
L's and P's last dumps; and no COB write in R's last continuation dump and Q's dump. A dump any
check did not pass on is kept, whatever else passed on it. **A run refuses a DLL older than its
sources** (`src` and `inc` `.c`/`.h` but the generated `git.h`, the two layout lists and the
`.allow` files): `<fixture>: <dll> is older than its sources (make -C tagpu/ddraw)`, and the run
exits 2 before any game starts.

#### S3c-2, verified and measured

MEASURED 2026-10-01 under Wine, scripted `tareplay run all -j 4` on private X servers; each game
ten minutes or less.

- **The final run, on the landing's code** (about 16 minutes): **207 checks, every one passes**,
  exit 0 — the nine fixtures' end blocks, `tarec check`, every dump's compare, every round trip's
  poison check (residue 0, 23 tables poisoned, 20 on the stock-limits build) and A-against-B, the counts (15 roots, 24 to 74 sink
  keys a fixture, the band and 32 rows: EQUAL in all nine), the images gate in all nine, the scan
  of 18 dumps, and no COB write on a puppet in 18 dumps. `recorder: 21 of 21 sinks counted` in every
  process.
- **The impacts the phases restored.** Before them, forest-circle over [661, 2941): the impact
  `0x499EB0` from the feature path (returning to `0x49B3D2`) 134 live, 111 on the playing, and the
  explosion, the sound and the flare inside it short by the same 23; every impact the playing
  missed had the tree in the recording machine's cell and its successor in the host's, at a
  projectile record equal on both but for its two unit pointers. With them, over the final run's
  windows, live/playing: **121/121** forest-fire, **68/68** forest-circle, **2491/2491** battle,
  **510/511** battle-los. Before the cells' unit slots went in ahead of the projectile tick,
  battle-los's slot-B impacts (`0x49B27A`) were 16 live against 19 on the playing; the final run's
  counts compare equal.
- **The first run** (before the owner's batch): 186 of 207 checks passed. The counts differed in
  eight fixtures, every difference traced to one of seven causes (the researcher's triage: the same
  shot reached through another engine path, owner-only queries, refused script indexes, debris and
  particles, the COB lag, the walk class after an unload, the lost impacts); the images gate failed
  in all nine (18 to 1,570 bytes, every run classified); the scan failed four dumps on two random
  words. The second run's two live forest games ended before Tlast (2550 against 3211, 1950 against
  2941); the third run's naval-push R process met O15's COB guard stop, the final run's did not.
- **The CRT's `rand` carries nothing a puppet host recomputes** (scout, DISASSEMBLED): of its 61
  call sites, every simulation consumer is carried, refused on a puppet level or never reached; the
  rest is presentation, the viewer's camera shake or the front end (the engine map).

#### What S3c-2 leaves

- **S3c-3**: `SIM_FEATURE` regions and their fixtures, the coverage build, `tareplay.py landing`
  and `broad`, the `replay-roundtrip:` note, CLAUDE.md's replay gate and Step 4b. Until then nothing
  runs the harness at a landing.
- **The COB one-tick lag (A5), fixed by the slice marker.** The live game's AutoAim starts
  AimPrimary inside the unit's turn and that turn's script slice runs it at T; a playing applied
  the recorded start at the replay step, after the whole unit tick, so its first slice ran at T+1,
  and the scripts it starts (RestoreAfterDelay, walk, `EMIT_SFX`) a tick later; a one-tick slice lag
  can re-phase a looping script for the rest of a unit's life (the level-start lag below). Since
  format 19 the recorder writes `RK_EV_SLICE` just before the slice of any unit whose COB had an
  engine start since the capture point, and a playing skips that unit's slice in the host's unit
  tick and gives it at the marker, after the recorded start (*The script slice*, below).
- **The level-start lag, fixed.** The playing made the level's own units after the host's first
  unit tick, so every COB thread of a level-start unit ran one tick behind for the whole level:
  tenseat's commanders' `MotionControl` chose walk/walklegs/stand 103/3/394 on the puppet against
  102/6/393 live over [481, 1411), and 109/0/445 against 108/3/442 over [301, 1291) (MEASURED
  2026-10-12, and reproduced call for call by `tools/tacob`'s VM fed the recorded inputs). The
  sinks' band hid it whenever the key was small enough. The playing now makes them at the first
  iteration's capture point, before the unit tick, and makes that capture point's write of the
  recorded globals after them, so their first slice also draws from the recording's simulation
  RNG (`tagpu_replay.c`, THE FIRST STEP'S UNITS). The `randprobe` fixture shows the RNG half: its
  commanders' scripts (`tools/randprobe_fixture.py`, loose `scripts/*.cob` in the instance's game
  folder, since a `.ufo` does not override a stock path) draw twice in their first slice and from
  then on call one of four scripts at a period both draws chose. MEASURED 2026-10-12 over
  [361, 1111): without the write, RandTick0..3 counted 133/258/163/0 live against 107/57/114/253
  on the puppet and the counts check failed; with it, every START and CALL key is exactly equal.
- **The in-game create lag, fixed by the slice marker.** A unit created during the unit tick in a
  later slot of a block not yet finished (a factory's product, a builder's nanoframe) gets its
  slice in that tick on the recording machine (the slot loop's per-slot re-read); a playing made it
  after the tick, so its COB ran one tick behind for its life. Its create's Create start is an
  engine start on its new COB, so its slice is marked after the create's events, and a playing
  gives it at the marker once it has made the unit (*The script slice*, below).
- **The walk class after an unload (A6), accepted.** The live game evaluates `0x43DA70` in a
  cargo's mover tick before its transport's later turn drops it; the playing's image-time walk
  sees the carrier already cleared and makes a StartMoving and a StopMoving the live game did not
  (MEASURED in carry-links' earlier runs: one each).
- **Debris and `EMIT_SFX` are rows, not replicas.** Flying pieces are spawned from the simulation
  RNG at EXPLODE, which a puppet host draws in another order, and the piece's smoke and fire are
  emitted by the explosion draw once a drawn frame; `EMIT_SFX` emits only in the viewed player's
  line of sight. Both sides' counts there are accepted as they come.
- **The forest fixtures' live games can end early.** The skirmish wind and the simulation RNG are
  seeded from the performance counter in every run, so a forest fire burns differently each time
  [INFERRED] and the level can end before Tlast, which leaves the fixture's later checks unrun.
- **Events outside a deferred bracket carry no phase.** A top-level event the recording made after
  the unit tick — a COB start from an impact's damage, a death from a typed kill-all — goes in at
  the replay step, before the host's projectile tick, and a death's own feature roots go with it,
  so a tree such a death fells is gone before the host's projectiles meet it: the lost impacts'
  class, not seen in the fixtures. A death must go in before the next image's units, which go in at
  the replay step; giving every event its phase is a format change. Its COB side: a StopBuilding
  the AI makes in the players phase goes in with step T's events, before image T+1's walk class,
  whose run-now start runs every record with `dt = 0` and so gives that start its first slice a
  tick before the recording machine did, where the walk class ran first (not seen in the three
  tenseat games simulated on 2026-10-12).
- **Within one phase the recording's roots go in after the host's whole call**, so a projectile
  later in the same projectile tick still meets a tree an earlier impact felled on the recording
  machine [INFERRED: battle-los's 510 against 511, inside the band].
- **A fresh-process seek missed radar contacts at a pass tick — closed by S3c-3.** Battle-los's
  seek to 1111 (1111 = 1 mod 30) left `+0x110` bit 8 clear on 2 aircraft (n7; 4 in the run before)
  that carry it in the recording and on the playing; the same GameTime passed in two earlier runs.
  It was not O14: the seek had dropped the one temporary sight record a playing held there, from a
  death of the viewed seat 19 steps before, and the radar pass's sight loop reads the stamp that
  record leaves in the viewed record's counters (*The seek's sight records*).
- **O15** — the COB guard's stop (`CORMSHIP.COB`) — still reaches naval fixtures' round-trip
  processes on some runs; it has a landing of its own.
- **The scan's residual**: the globals image's random word, `rngseed`, is `opaque` and passed
  over; a dump made with another `replay-globals.tsv` than the checkout's is scanned whole, and
  there the seed can still land in the main block [INFERRED: about one in 9,000].
- **The images gate's own limits**: a bit equal at both ends of [T1, Tlast] on each machine counts
  as unwritten in play; the twin covers the camera, the zoom, the cursor and the in-game menu, not
  every screen a viewer can open.
- **Sink counts are in one perspective only**, the recording viewer's.

### S3c-3 — fixtures beside the code, and the gate

What turns the harness into a landing gate: every line of simulation code belongs to a declared
feature that names the fixtures exercising it, a build that records which functions ran, and a
command that runs a landing's fixtures and proves they reached what it changed. The tiers and the
checks as asserted are the completeness note's
[*replay test harness*](demo-recorder-completeness.md#the-replay-test-harness). The seek's
temporary sight records, which S3c-2's reruns found missing, are part of this landing too.

**The seek's sight records** (`tagpu_replay.c`, *THE SEEK'S SIGHT RECORDS*; O9 reversed, *Decided
2026-10-01* below). When a live unit of the viewed seat dies under true LOS, the destructor makes a
temporary sight record for 60 ticks through `0x482910`, and its stamp in the viewed record's sight
counters lets the radar pass's sight loop (`0x46780E`) give contact bit 8 to enemy units over it
(the engine map's [*temporary sight records*](../exe-reverse-engineering.md#the-demo-recorders-globals-projectiles-and-seek-disassembled-measured-2026-10-01)).
R4 drops every record and R5's refill leaves none, so the seek makes again the ones a playing
holds at T, from the deaths the recording carries — each `RK_EV_DEATH` already holds the victim's
image as the destructor found it, so no format change was needed.

- **The window.** `recr_seek` takes a lead and a visitor: it starts from the last snapshot more
  than `EYE_LEAD` = 123 iterations before T (two lifetimes and the window) and calls `eye_visit` on
  every iteration it reads up to T. `eye_visit` follows the engine's rules: an entry for each death
  in step k of a live unit whose owner's `+0x146` is the viewed record, under that iteration's
  LosType bit 1; none past 20 (counted); all dropped at a change of LosType bits 0–2, as a playing's
  LosType write drops them; after each step before T−1, those whose expiry is below it, as the
  expiry after the tick loop (`0x495688`) removes them. A record from before the lead has expired
  before the window and can change a later decision only through the cap of 20, so the entries are
  a playing's unless the simulated cap refused a death, which is counted and logged.
- **R14a**, after R13's LosType and before R14's fog composite and R14b's radar pass: each entry
  through the engine's add `0x482910` with the def's `+0x202` and `+0x170` (the def bounded by
  `UNITINFOCount` through `addr_of`) and the ticks it has left as the duration; a made or refused
  record is told by the engine's count. **R14c**, after R14b: the expiry `0x482130` once, as
  `0x495688` follows the players phase. The invariant is a lifetime: the add makes each record with
  its stamp and only the expiry removes both, so the counters stay the engine's own; nothing after
  R14a rebuilds the viewed record's counters (R14 writes V's alone, and V owns no unit).
- **R14d**, after R14c (a takeover's after TK4a's deselect): the viewed record's minimap picture
  `0x466DC0`, which a playing rebuilds every tick (the players phase for a seat, `viewer_frame`
  for record 10) and a paused frame does not — display state, the viewer's own.
- **The seek's line** gains `temporary sight records: <m> made again, <r> refused, <c> deaths past
  the engine's 20 (the records may differ from a playing's)`.
- **`tarec.py compare-dumps` is strict again**: O9's fold is deleted, and `replay-globals.tsv`'s
  `tempsightcount` and `tempsightblock` rows (still `leave`) say why the records are not carried.

**Features beside the code** (`inc/tagpu_simreg.h`, `SIM_FEATURE`). A feature is every line of a
`sim` file from its `SIM_FEATURE(id, puppet, "fixtures")` to the file's next one; `fixtures` names
entries of `tools/replay/fixtures.json`, and `puppet` says what the feature does on a puppet host:
`SIM_PUPPET_RUNS` (its code runs there and its effect must match the recording's),
`SIM_PUPPET_OWNER` (its own code refuses to act on a puppet; the owner's effect arrives through the
recording) or `SIM_PUPPET_INDEPENDENT` (it changes nothing a replay shows; the review checks the
word, the harness does not grade it). The link runs `tareplay.py features --check` after the
state check and fails by name: `UNFEATURED` (code or a declaration above a file's first feature),
`MALFORMED` (not one line at the line's start, or an unknown puppet word), `DUPLICATE` (two
features of one name), `UNKNOWN` (a fixture `fixtures.json` lacks), `NOFIXTURE`. The attach line
counts them (`features: <n> run on a puppet, <n> the owner's, <n> independent, <n> malformed`).
`tools/replay/tafeat.py` holds the parser — a small C function finder over the source with
comments, strings and preprocessor lines blanked — and reads the linker map and the coverage files.

**58 regions**: `tagpu_patches.c` 48 — one per installer for the most part (a region ends at its
`fix_*` and keeps the log formatter after it; B7's four installers share `b7_hits`), with the
file's shared parts in regions of their own (the patch helpers at its head, `tagpu_apply_patches`,
the limits' pools and sites, the COB guard, the seek's restore hooks); `tagpu_datakeys.c` 7, its
numbered sections; `tagpu_weapons.c` `weapon_slots`, `tagpu_puppet.c` `puppet_host` and
`tagpu_cobtrace.c` `cob_trace`, one each. Every one is `SIM_PUPPET_RUNS` but `cob_trace`,
`SIM_PUPPET_INDEPENDENT`; none is `SIM_PUPPET_OWNER`.

**The coverage build** (`make COVER=1`, `ddraw-cover.dll` and `ddraw-cover.map`; `make
LIMITS=stock COVER=1`, the stock-limits twin; `-DTAGPU_COVER`, objects `.cover.o`). The `sim` files
and the recorder's own four are compiled with `-finstrument-functions -finstrument-functions-once
-ffunction-sections`: each function calls `__cyg_profile_func_enter` the first time it runs, and
`tagpu_cover.c` marks its first byte in a bitmap over the DLL's `.text`, bounded by the section
header. Hooking every call made the forest fixtures' live games miss their deadlines; once a
function keeps them at speed. The map places each function in a `.text$<name>` section, so an
address names (object, function), a compiler clone under its function's name. The trigger's `cover
<name>` writes `replays\cover-<name>.bin` at once, and the first name armed is written again at the
level's end (`tagpu_rec_level_end`), so a harness that asks while the level plays gets what ran up
to the end and collects the files afterwards. `sim-state-check.sh` passes over the `-once` guards
(`C.<n>`) in coverage objects only. No instrumented file holds a naked function or inline assembly.

**How the regions and their fixtures were chosen: by measurement.** `tareplay.py measure` records
and plays each candidate (L and P only) on the coverage builds and writes the functions each
reached. 62 candidates — the nine fixtures and the `b*`, `c*`, `cob-*`, `wpn-*` and other scenarios
of `scenarios/` — of which 41 ran; of the 21 that did not, the forest pair missed their deadlines
on the every-call build and the rest could not load (content not installed, a map not present, a
refused placement, the unit limit). A second measurement ran 18 of them on the once build with
their content generated: all ran. `b1-resurrect` (a refused feature placement) and `ball10` (the
unit limit) never ran. Per region, the functions nearly every run reached (all but two runs at
most) need no particular fixture; the rest get a greedy cover — the run that reaches most of what
is left, a fixture preferred — and `battle` where nothing is left. Eight scenarios became fixtures
(17 in all): `b7-radar-hail`, `b7-word-outright`, `c2-weapon-keys`, `c3-reclaim-bound`,
`c3-veterancy`, `c4-kill-all`, `c4-survivor` and `wpn-badtgt`, each with a `content` command that
writes its mod into the game folder at every launch (`tools/datakeys_fixture.py`;
`tools/extra_weapons_fixture.py` now takes the folder to write into). 46 regions name `battle`
alone.

**What no fixture reaches.** The measurements left functions in 27 regions that no run reached,
by class: the network-only paths (the receivers and senders of `wire_*`, `hit_*`, `ghost_*`,
`kill_rx_*`, `tx_rx_*`, `wpn_tx_*`/`wpn_rx_*`, and the wind's DirectPlay session seed); error and
refusal paths (the COB guard's rejects and faults, `lim_no_stub`, `lim_needs`,
`lim_types_refused`, the defect fixes' fallbacks and diagnostic events); the saved game's save and
restore (`cob_save`/`cob_load`, `save_word`/`restore_quiet`, the saved features and records); the
build ghost's piece masks (`keys_ghost_mask`); the queued-site test; `order_anyone`; the COB trace;
the weapon slots' diagnostics; the puppet gates no fixture triggers (`gate_attach`,
`gate_drop`, `gate_transfer`, `gate_save`, `gate_debugkey`); and a handful of others (the terrain
window's draw, parts of the composite scratch, the map list's terrain test, a veterancy label). `measure` runs L and P only, so the
seek's and the round trip's own functions are not in its lists; a landing's R and Q processes
reach them. A landing that changes an unreached function fails `UNCOVERED`, and the owner decides
each time (OQ7); there is no standing list.

**`tools/replay/tareplay.py`'s commands** beside `run` and `list`:

| Command | What it does |
|---|---|
| `landing [--base main] [-j N] [--keep] [--out DIR]` | the landing tier: from `git diff main...HEAD`, a changed line of a `sim` file selects its feature's fixtures (a change only in comments or whitespace selects nothing; a changed `#define`, string or character literal is a changed line; a line no feature covers is `UNFEATURED`), a changed header a `sim` file includes every feature of that file whether or not the file changed too, the recorder's own files, `tools/replay/` and `scenarios/replay-end.json` every fixture, a changed fixture entry, a scenario a fixture loads or the tool its content runs that fixture; the old side is the merge-base, as `git diff`'s is. It runs them on the coverage builds and fails `UNCOVERED <file>:<function>` for a changed function none of them reached (a changed file-scope line needs one function of its region), in every build that compiles the changed line: a line under `#ifdef TAGPU_LIMITS_STOCK` adds `stock-limits` and must be reached by a fixture of the stock-limits build, a line under `#ifndef` by one of the default build (`tafeat.builds`). A gated change that selects nothing to run (comments only) passes with a note of `fixtures=-`. On a pass it writes a git note on the commit it graded, taken when the run starts: `replay-roundtrip: dll=<describe> range=main..<sha> features=… fixtures=… seeds=… reached=<k>/<n> result=pass date=…`. It refuses uncommitted changes under `tagpu/ddraw`, `tools` or `scenarios`. |
| `landing --check` | exit 0 when nothing gated changed, or when a passing note sits on a commit R of `main..HEAD` and `R..HEAD` changes nothing gated; 1 otherwise, saying why |
| `broad [-j N] [--release DLL]` | the broad tier: every fixture, three round trips each, and `fixtures.json`'s `broad` entries, which no landing runs — `limits-tier1` (4 × 1,500 units, `unit_limit` 1500) with 20 round trips, and `cost-four-ai`, a ten-minute four-AI game recorded live and reported, never failed (OQ5), for the capture's cost an iteration and the file's size per hour of game time — then UNCOVERED over the tier — a feature none of its fixtures reached, a root or sink site no run counted, a recorded table no recording changed between T1 and Tlast. `--release DLL` runs that DLL instead of the coverage builds, without reach |
| `measure SCENARIO… [--end T]` | L and P of each scenario on the coverage builds, to GameTime T (default 1800; a fixture to its own end), and the functions each reached (`reached.json`) |
| `features [--check DIR]` | the regions; with `--check`, the link's check |
| `run --cover` | `run` on the coverage builds, each process writing its `cover-<proc>.bin` |

None of them builds: a run refuses a DLL older than its sources and names the `make` line
(`make -C tagpu/ddraw COVER=1`, and `LIMITS=stock COVER=1` for `stock-limits`).
`tools/replay/test_tareplay.py` holds seven tests: the comment blanking, the function finder, every
parsed function in the map, the regions, `UNFEATURED` and `MALFORMED`, a comment-only change, the
check.

**The gate.** CLAUDE.md's *Simulation changes stay replayable* gains the bullet "A landing that
changes simulation code runs its replay fixtures", and the landing command gains Step 4b, the
replay gate: when the diff touches a gated file, `tareplay.py landing --check` must pass, otherwise
`tareplay.py landing` runs on HEAD; a game that cannot start at all is escalation reason 3, and an
`UNFEATURED` line or an `UNCOVERED` function is fixed with a declaration or a fixture, or landed
as the owner decides. The review brief of Step 5 asks for each new or moved region and whether its
fixtures reach it.

#### Decided 2026-10-01

- **O9 reversed for the seek**: the seek makes the viewed seat's live temporary sight records again
  from the recorded deaths before T through the engine's add, then runs the engine's expiry;
  `tarec`'s fold is deleted. No format change.
- **Two residuals of the sight records accepted**: the cap of 20, whose refusals are counted and
  logged, and the expiry's once-a-frame timing (*What S3c-3 leaves*).
- **OQ7, per landing**: a landing whose changed function no fixture reaches goes to the owner each
  time; there is no standing exemption list.
- **OQ9, adopted as written**: the CLAUDE.md bullet and Step 4b.
- **The debris flares accepted** (c4-survivor): wreck debris, whose velocities EXPLODE `0x481140`
  draws from the simulation RNG, lands at other ticks on a puppet, on ground (the explosion add's
  4th argument 0, so the flare `0x472630` fires at `0x420AE1`) or in water (1, no flare). Over the
  debris period every simulation value compared EQUAL; the flares' totals matched over the whole
  game, but a counting window that began inside the period differed by up to 4. The counts now key
  the flare by the add's caller (`tagpu_recorder.c`, `sink_472630`), and four `replay-absent.tsv`
  rows accept the falling records' (`0x4210A1`) and the flying pieces' ground explosions'
  (`0x4214D9`) flares, the class OQ3f accepted for the explosions themselves; the band applies per
  add caller.
- **`sk_back` lands UNCOVERED** (this landing, per OQ7): the reader's way back after a refused seek
  changed only in the arguments it passes (`lead` 0, no visitor); no fixture refuses a seek that
  way.
- **The fixtures' manual steps are a follow-up**: three of the promoted fixtures leave out steps
  their scenarios describe (*What S3c-3 leaves*).

#### S3c-3, verified and measured

MEASURED 2026-10-01 under Wine, scripted runs on private X servers; each game ten minutes or less.

- **The seek's sight records.** Fresh-process seeks to 1111 on the two battle-los recordings whose
  seeks had missed contacts (n7, n6) each made 1 record again, 0 refused, 0 past the cap, and
  compared EQUAL to the live dump and to the playing, the sight counters and contact bits included.
  One playing with round trips at 1111, 1141 and 1831 (the last's lead crossing the snapshot at
  1800): A against B strictly EQUAL in all three with the fold deleted, 1, 1 and 0 records made
  again, seeks 212, 215 and 540 ms. A round trip at 3831 made none; its one difference, unit 504's
  `+0x110` bit 16, is not a bit the radar pass or the records write [INFERRED to be O15's class, not
  re-measured here]. `tareplay run battle-los forest-circle` (forest-circle for the add's circle
  path): exit 0, 46 of 46 checks.
- **The debris flares.** `tareplay run c4-survivor` after the flare's key and rows: exit 0, 23 of
  23 checks, and the live log says the flare is keyed by its add's caller. Over the debris period, a
  dump every 30 GameTimes live and on the puppet from 91 to 451 (13 pairs of dumps): `compare-counts`
  EQUAL over all 78 windows; the flare's keys at 451, by caller, are the falling records' 17, the
  pieces' 2, five of EXPLODE's six adds 1 each and the impact's 3, and no flare is counted
  without one.
- **The measurements**: 41 of 62 candidates, then 18 of 18; 619 to 775 functions of the
  instrumented files reached per scenario.
- **The landing's own tier.** Every feature being new, `tareplay.py landing` ran all 17 fixtures
  on the coverage builds, four games at once, at the landing's last code commit: 391 of 391 checks
  passed — every end block and `tarec check`, every L dump against the recording, P, R and Q
  against it as puppets, the counts and the images gate of all 17, the address scan, the COB
  writes, and 51 round trips, each with residue 0 and A against B EQUAL, their seeks 7 to 514 ms.
  One changed function no fixture reached, `tagpu_replay.c`'s `sk_back` (the reader's way back
  after a refused seek, changed only in the arguments it passes): landed UNCOVERED by the owner's
  decision, so no `replay-roundtrip:` pass note was written; the run is recorded in a note of its
  own on the commit.

#### What S3c-3 leaves

- **The cap of 20 is untested.** No run has reached it (battle-los's viewed seat loses at most 8
  units in any 62-step window outside its closing scenario), so the path where the simulated cap
  refuses a death is counted and logged but has not run; there the records may differ from a
  playing's.
- **The engine's expiry runs once a frame**, after the frame's whole tick loop (its one call,
  `0x495688`): inside a frame of several steps, a capture point still holds records whose expiry
  has passed, and their stamps are in the viewed record's counters, on the recording machine or on
  a playing, while the seek's window expires per step. So a capture-point dump taken inside such a
  frame (L against P, or a fresh seek's) can differ in the viewed record's sight counters and in
  the contact bits until the next pass; a round trip's frame dumps cannot, being taken after the
  frame's expiry (`0x49693E`, before the draw `0x4969CD`).
- **A recorded LosType change still drops the records** on a playing (*What S3b leaves*).
- **Reach is per function, not per branch**: a changed branch inside a function its fixtures entered
  is not proven taken, and a function is marked on its first entry only.
- **What no fixture reaches** fails a landing that changes it, by name, until network games are
  recorded or a fixture reaches it (OQ7).
- **Three fixtures leave out their scenarios' manual steps.** b7-word-outright's Ctrl+D
  self-destruct, c4-kill-all's `+kill` chat commands (which need cheats on) and c4-survivor's ATLAS
  flight to the AA gun with the TXLLT2 after it: the harness can type a chat line and give an
  order, and its `keys` steps deliver native input. These three entries still omit
  the manual steps; dedicated `secondary-orders` fixtures exercise native Ctrl+D
  on both builds. The three entries were chosen for what they reach without the steps.
- **`limits-tier1` and `cost-four-ai` have not run in the broad tier yet**; the broad tier's first
  run is their measurement.
- **The debris pools' class is open**: `s_expl`, `s_psys` and `s_aux` are declared `SIM_STATE` and
  are not carried; the debris they hold writes nothing the simulation reads, and their class is the
  owner's decision.
- **Only the `sim` files and the recorder's own are instrumented**; a change elsewhere runs no
  fixture unless it touches a gated file.
- **O15** still reaches naval fixtures' round-trip processes on some runs, and the forest fixtures'
  live games can end before Tlast (*What S3c-2 leaves*).

## S4 — rewind, takeover and forks

S4 is four landings, in this order:
- **S4a** records B4's per-slot columns.
- **S4b** is the takeover from a recording, with its fork written into the file.
- **S4c** adds the fork rules: three forks, the least recently used deleted, shared history kept.
- **S4d** is the live rewind on the recording machine: Back to now and Play from here.

A takeover is the seek with a takeover mode. The owner's decisions of 2026-10-02 are in the
[completeness note](demo-recorder-completeness.md#decisions-taken).

### S4a — B4's per-slot columns

**Decided.** The stale-hit table's three columns are recorded as tables of their own: `stamp`,
`freed` and `local`, one u32 a unit slot (`tagpu_patches.c`, beside `hit_layout`). A fork reads
them across its takeover point:
- C4's carried-blast pick `tx_mark_held` compares the recorded `s_txMark` with the slot's stamp;
- B4's two-tick hold `hit_held` reads the slot's free.

Without them, a keyed passenger whose transport died in the step before the takeover would
explode uncarried. The table's own row keeps its `SIMF_COLUMN` fields, so its count stays a kept
count and no byte has two declarations.

The arguments:
- **The lifetime:** the column pointers point into the table `s_hit` was published with, and that
  table is never freed.
- **The bound:** their count `s_hitRows` is that table's n (n ≤ room). It is a heap table's count,
  which the seek never writes, so the seek writes at most n rows.
- **The ordering:** `hit_reset` publishes the pointers right after `s_hit`, on the same thread.

Each column starts on a page of its own and shares none: the recorder write-watches each as a
table, and a reset of a shared page would hide one table's writes from the other. The recorder
diffs whole any heap table whose rows do not start on a page.

A puppet host reads and writes them as the engine runs: a replayed create reads its slot's stamp
(`hit_take`), the destructor's cargo loop reads the stamps (`tx_cargo`), and a free writes
`freed`. Replayed deaths never reach `tx_arm`, and everything a puppet derives from the columns is
itself a recorded table, which the step writes again at the next boundary. `local` is the delay
lever's oracle, which the simulation never reads; it is recorded so that a fork keeps it.

**Verified and measured** (2026-10-02). The first landing run failed, and the failure was the
landing's: the columns started 20 bytes into the table's first page. The write watch reports whole
pages, so the recorder measured each column's first page from below its base and never emitted
its rows. The stream carried 28 corrections, and a frame dump differed in 8 dwords, all in
`s_hitFreed`'s first 1019 slots. With the columns on pages of their own, the landing tier ran all
17 fixtures on the coverage builds:
- 391 of 391 checks passed;
- 51 round trips, each EQUAL with residue 0 and 26 tables poisoned, the three new ones among them;
  seeks took 16 to 595 ms.

The battle's recording grew 0.8 % (83.5 MB against S3c-3's 82.8 MB, games not identical).

### What the Replays browser asks of a recording

S5's browser shows a recording's map, mod and result without walking its stream (the S5 note's
*What S5 asks of the recorder*, items 1 to 3, on the S5 branch until it lands). This landing
comes between S4a and S4b, because S4b turns the file into segments. Every addition is a tag or a
block an older reader skips, so `REC_FORMAT` stays 4.

**Decided:**
- **The minimap** is an `RB_MINIMAP` block right after the header, packed like the other blocks. It holds the TNT's
  picture, the GAF frame at `main+0x1426B`, as w×h palette indices, plus the box `main+0x142EB`,
  `+0x142ED`; the engine shows the part [0, 2·columns) × [0, 2·rows).
  - LoadMap stores the frame (`0x483900`), and only the teardown frees it (`0x483DFE`).
  - The plane is uncompressed, in a block the engine sized w·h + 0x18 from the same w and h
    (`0x4B8DA0`). The recorder reads it only at `frame + 0x18`.
  - A map without a picture has no block, and neither does a picture of more than
    `REC_MINIMAP_MAX` indices, which a ring record cannot hold.
- **The mod** is three header entries:
  - `RT_GAME_VERSION`: the main menu's version text at `0x5031B4`, which a mod's patch may
    rewrite; printable ASCII, at most 7 characters, absent otherwise.
  - `RT_ARCHIVE`: one per archive the engine mounted, in mount order, by file name (never the
    path, which names the player's folders), size and mtime. No file's contents are read.
  - `RT_MODPRINT`: the FNV-1a of every mounted archive's name and size, and the engine's count of
    them. Two details:
    - Impure's own `impure-patch.ufo` is left out, so an Impure update does not make old
      recordings look like another mod's.
    - The files' times are left out, so two installs of one mod agree.
    `tagpu_rec_mod_print()` computes the running game's the same way. A map archive changes the
    fingerprint, so it names an install, not whether a recording can be played: that is
    `RT_UNITDEFS`'s and `RT_WEAPONS`'s to say. `RT_ARCHIVE` lists the first `REC_ARCHIVES` (64).
  - **Departures from the ask:**
    - Sizes, not CRC-32s: hashing an install's 900 MB of archives is not for the game thread. The
      S5 session agreed, and will ask again if two builds collide.
    - The last commander death a seat suffers, not each one.
  - The list is `[0x51FBD0]+0x618`, count `+0x61C`. Only `InitTAHPIAry 0x41D4C0` mounts, prunes
    or reallocates it, from its eleven callers on the game thread, the skirmish's Start last.
    The recorder reads it on that thread at the level's first iteration, so the list cannot move
    under it.
- **The end** is two end-block entries:
  - `RT_GAME_END`: bits 2, 4, 5 and 6 of `main+0x3923B`, a u16, which still hold the controlled
    player's result at the teardown's entry: bit 4 won, bit 6 lost, bit 2 without them a quit. It
    is 0 when the recording ended before the level did.
  - `RT_SEAT_END`, one per seat: its result, the GameTime its last commander died, and, when it
    ends the game with no unit, the GameTime its live count last reached 0. The last, not the
    first: a scenario's load kills every commander and gives the seats new units (the fixtures'
    recordings had every seat "defeated" at GameTime 34), and a Deathmatch respawns a commander
    after an elimination.
  - The commander test is `Send_UnitDeath`'s own (`0x4864CB..0x48650F`): the UnitName against the
    side's commander name, made at the destructor's entry, with the def bounded by
    UNITINFOCount and the side by the sides loaded. Elimination is read at the destructor's
    exit: its decrement (`0x486DFC`) took the owner's `+0x144` to 0.
  - The engine holds one result, the controlled player's, and no draw. The other seats follow
    the rule in `tagpu_recfmt.h`: a seat that ends the game with no unit lost; in the controlled
    player's victory it and its allies won and every other seat lost; anything else is unknown.
- **`RK_END` carries the seats.** The ring's reserve is sized from it, including the tail of the
  ring an `RK_END` skips when it does not fit there; that tail is at most the record less 4
  bytes, as the old 52 bytes were for a 28-byte `RK_END`.
- **`recr_info`** reads all of this from the header, the minimap block and the end block. It
  refuses a recording of another format, and reads only each other block's 32-byte header to pass
  it. `tarec info` prints it.
- **A seat** is a record the header names (`RT_PLAYER`), so its entry survives a controller that
  changes during the game.

**Verified and measured** (2026-10-02). The landing tier ran all 17 fixtures on the coverage
builds: 391 of 391 checks passed, with 51 round trips EQUAL (seeks 7 to 577 ms). The recordings
carried the new fields:
- the 252×252 minimap, with the shown part (252×248 on the battle's map);
- the version `v3.1`, 31 or 32 archives mounted, and their fingerprint;
- each seat's entry.

`recr_info` was run in a scratch build under Wine on those recordings. It read the same values as
`tarec info`, down to the sum of the minimap's 63,504 indices, and an older recording's empty
fields.

The first run named the end's own functions `UNCOVERED`, because the level end wrote coverage
before the recording ended; it now writes it after. Five functions no fixture reaches, because
only S5's browser calls them, landed `UNCOVERED` by the owner's decision of 2026-10-02:
`recr_info` and its three helpers, and `tagpu_rec_mod_print`.

**Left:** S5's fourth ask, starting a recording mid-game and stopping it on request,
remains separate recorder work. S4d suspends and resumes an existing recording.

### S4b — the takeover and its fork

**Built.** A level that plays a recording becomes a live skirmish at GameTime T, with player P human,
every other recorded seat an AI and the viewer's seat dropped, and the game after T is recorded as a
new timeline appended to the same file. The takeover is the seek in its TAKEOVER mode; the steps, the
invariants, the file's segments, the fork's handle and the residuals are in
[the takeover note](demo-recorder-s4-takeover.md).

**Decided** (2026-10-02, with the owner):
- **D1**: the order nodes restart in the constructor's state, the builders adopt their frames, and
  Paralyze and BuildWeapon resume (OQ-A). VTOL_MobileBuild's orphaned frame and the adopted builders'
  COB pose are accepted residuals.
- **D2**: `tarec compare-fork` excludes the node state D1 restarts, the records outside the fork's
  seats, the roles that changed and the contact bits by the puppet view's rule, and nothing else.
- **D3**: a fork is always recorded; a takeover whose file cannot be appended is refused before the
  rebuild.
- **OQ-B**: the mixed-side interface after a takeover of the other side from a running playback is
  fixed by a mid-level re-skin, its own landing after S4b; `tacli replay takeover` launches with the
  side viewed being P's.
- **OQ-C**: the takeover run's thresholds as `tareplay.py`'s `TAKEOVER_CHECKS` list them.
  S4d applies the owner's direct AI activity rule to this fork check: every AI seat must issue
  fresh planner orders that reach normal dispatch in an armed 1800-tick window, and at least
  25% of the AI seats' mobile units must move more than 8 wu. Construction is reported only.
  The window begins after takeover, armed while the engine acknowledges pause, and K waits
  for its completion before ending the game. The original timeline's building choices are
  not the liveness criterion.

**The fixtures.** `battle` is also taken over at T2 as its controlled player, the line within
2000 ms, and its first ARMCK ordered 300 wu east from its base: the order's unit stands in open
ground, since at T2 every ARMPW of the army is in the melee, where one ordered sideways moved 47 wu
in 12 s. `takeover-end` (Show Down, slots 2 and 3 closed) is taken over as 0 and ends won after
`+iwin`, the console's IWin: its kill-all for the AI and the game's won and stop bits. `+kill 1`
cannot end it, since the console's Kill disarms the objective after its kill-all (`0x4164EF`; the
[engine map](../exe-reverse-engineering.html)'s kill-all callers), so the fixture does not reach
the objective's own victory test that the takeover re-arms. `tenseat` is taken over as its
controlled player at GameTime 901, the entry's `at` in place of T2: the tick after its typed LOS
command, where the takeover's contact bits must come from the recording (O14, *What S3b leaves*).
Each runs K (the takeover, its checks and a dump) and F (K's file played from T −
300 across the fork) after the usual L, P, R and Q.

**The layout's hash.** The `order 0x006 ondestroy` row now names all nine handler functions that arm
the destructor's call, which changes `TAGPU_REPLAY_LAYOUT_SHA256`: recordings made before S4b refuse
to play. The recorder is unpublished.

**Verified** (2026-10-02): the landing tier, 414 checks passed, the three takeovers and the
reader's damaged-file paths; the numbers are in the takeover note's *Verified by*
(`demo-recorder-s4-takeover.md`). Seven error paths landed `UNCOVERED` by the owner's decision.

### S4c — the fork rules

[The fork rules](demo-recorder-s4-forks.md) describe the complete metadata table
(`RB_TIMELINES` = 6), three-live-fork LRU policy, playing-end touch, explicit
delete/export, and worker compaction with shared ancestor prefixes. The original
is excluded from automatic eviction but may be explicitly deleted. Deleted IDs
cannot be selected and the highest allocated ID is never reused.

`tacli replay forks` lists and manages the instance's file, and takeover
`--dry-run` predicts eviction. `tareplay.py forks battle` tests the rules; the
landing tier adds X after the existing lanes on both battle and stock-limits.
Live rewind remains S4d; the browser controls remain S5.

On the reference Wine setup (2026-10-02), the full coverage run plus the corrected
forest-circle rerun passed 466 checks across all 18 fixtures, including eleven
fork checks on each build and 54 round trips EQUAL. Coverage reached 48 of 49
changed functions on every required build; `ring_free` remains UNCOVERED, approved
for this landing by the owner under OQ7 on 2026-10-02. The earlier naval sink-count difference also
reproduced on pre-S4c main; its cause remains open and the tolerances are unchanged.

### S4d — live rewind

[Live rewind](demo-recorder-s4-rewind.md) provides `tacli rewind` open, seek,
return-to-now, fork and close on a recorded skirmish. The pause guard precedes
the engine's scheduler test; the recorder suspends capture and events while
retaining its pending iteration. Atomic frame dumps compare a historical frame
with a fresh seek and the restored present with its saved frame. The original
writer closes before a fork can acquire its file.

Normal-limits smoke checks include exact fresh-seek equality, filename-collision
handling, append refusal/retry and exit during rewind. Both coverage builds pass
28 rewind checks, including preservation of objective switches and radar contacts
on return, and new planner orders reaching normal dispatch for every AI seat
after both return and fork. The native disabled/re-enabled AI control verifies
that the activity check rejects a missing planner. Construction is reported only.
The module note records the approved unseated sight, rewind-only VM child-count
and bounded naval flare-count exceptions. Exact COB execution continuity is
outside S4. The naval COB stop and failed-worker cleanup are corrected. The
full landing tier passes 570 checks across 20 fixtures, including 60 exact round
trips, with all 62 changed functions reached on every required build.

## The script slice (format 19)

A COB thread's only clock is the unit tick's `DoScriptsNow(1)` (`0x48ADEB`, its only call), so a
start reaches its first slice at the first slice of its unit after it. On the recording machine
that slice comes in the unit's turn, after everything earlier turns and the unit's own AutoAim
made; a playing's host gave every slice in its own unit tick, before the step's replayed events,
so a start made before the slice (A5's AutoAim starts, a create's Create in a later slot) got its
first slice a tick late, and a looping script stayed re-phased for the unit's life.

- **The stub.** The unit tick's 17-byte script call `0x48ADDF..0x48ADF0` jumps to a stub that gives
  the slice from C (`tagpu_patches.c`, THE SCRIPT SLICE; site facts in the
  [engine map](../exe-reverse-engineering.md#the-cob-modules-entries-every-start-outside-a-slice-and-the-script-slice-disassembled-2026-10-12)),
  in the fail-closed table of both builds, on every level. It counts each slice at the root
  `0x48ADEB` by where it was given: aux 0 in the unit tick, 1 at a recorded marker, 2 at a
  playing's step's end.
- **The record.** `RK_EV_SLICE` = 57, `{u32 unit ID}`, in `RCS_EVENTS`, its ID coded in `C_ID`.
  The recorder writes it just before the slice of a unit whose COB object was the `ecx` of a call
  to `0x4B0B00` or `0x4B0940` since the capture point: two observers on those entries key a set by
  the COB's address, emptied by an epoch at every capture point (`tagpu_recorder.c`, THE SLICE
  MARKERS). Outside a slice those two are the only ways a COB object gets a thread. A set past half
  full marks every slice for the rest of the step: a superset is exact, since moving a slice whose
  COB had no start changes no COB state. The writer and every reader refuse a marker inside a
  feature or death bracket or outside the slots; at most one per slot per iteration.
- **The playing.** At the capture point the iteration's markers are planned; the host's unit tick
  skips a planned unit's slice; the marker in the replayed events gives it, after the recorded
  events that preceded it; a unit the step creates gets its slice at its marker once its create is
  made. A debt its marker did not pay is given at the step's end, counted and noted; a create or a
  death in the slot voids it (`tagpu_replay.c`, THE SCRIPT SLICES). A playing's log ends with
  `replay: script slices: ...` (planned, deferred, given at the marker, at a create's end, at the
  step's end, and the anomalies).
- **The check.** The count root compares per aux exactly, and `--cob-exact`
  ([completeness](demo-recorder-completeness.md#the-replay-test-harness)) compares the VM's START
  and CALL children exactly on `slice-walkers`, `battle`, `tenseat` and `codec-decline`. The
  fixture `slice-walkers` (`scenarios/replay-slice-walkers.json`: three AI seats on Core Prime
  Industrial Area, each with a lab and two constructors beside it, and every 600 ticks a wave of
  eight CORAK and eight ARMPW ordered at each other on seat 2's plateau,
  `replay-slice-walkers-wave.json`) makes units in later slots during play and kbots that walk and
  aim. MEASURED 2026-10-12: on Landing A's DLL its counts differed (RestoreAfterDelay 1077/1073,
  walk 3271/3268, walklegs 1025/1028 live/puppet over [1141, 3481)); on this landing's, over
  [631, 2881), its recording held 1,548 markers -- 17 after the unit's create in the same
  iteration, 1,408 after a START or a SHOT, 103 after a named start, 20 after nothing recorded --
  and the P lane's playing, over its whole run from GameTime 1 to its last dump at 2881, gave all
  2,045 it planned at their markers, none at the step's end (the two windows differ: the
  recording's markers before 631 are in the second count and not the first).
- **A replayed death's Killed** runs as the recording machine ran it, as a query
  ([completeness](demo-recorder-completeness.md), *COB slices a playing gives one tick late*).
- **What it leaves**: the walk class's run-now pass, seeks and round trips (fresh COB from
  Create), A6, mid-tick reads, a unit dying before its turn, starts made inside a slice by a
  replayed state change, and on a multiplayer recording the ledger's starts and
  other owners' units (the [completeness note](demo-recorder-completeness.md), *COB slices a
  playing gives one tick late*).
- **The owner's queries** (`RK_EV_QUERY`, kind 58). `slice-walkers` was the first fixture to reach a
  lab's `QueryBuildInfo`, a query only the factory's order handler makes, so a puppet never did
  (MEASURED in the landing gate, 2026-10-12: live 9, puppet 0 over [1051, 2671); 8 against 0 on
  Landing A's DLL). `QueryNanoPiece` was the same: an order handler's, owner-only, its count
  accepted as `absent` since 2026-10-01 (battle: live 841, puppet 0). The recording now carries
  both where the engine makes them and the playing makes each through the same `0x4B0BC0` at its
  place ([the engine map](../exe-reverse-engineering.md#the-owners-cob-queries-disassembled-2026-10-12)),
  and the `QueryNanoPiece` row is gone. MEASURED 2026-10-12 on `battle`, `slice-walkers`,
  `codec-decline` and `tenseat` under `--cob-exact`, every lane passing: the recordings held
  1,675, 15,665, 140 and 3,864 queries, every playing made all it reached (none refused), and
  both keys' counts are equal live and puppet in every window (`slice-walkers` over [1171, 3121):
  `QueryBuildInfo` 44 against 44, `QueryNanoPiece` 8,554 against 8,554). `QueryTransport` and
  `QueryLandingPad` are not recorded: an open gap no fixture reaches.

## S5

**S5 is planned in [its own note](demo-recorder-s5-ui.md)** (2026-10-01): six landings, the screens
drawn by the engine's GUI with our own art per style. It asks four things of the recorder, which
stay the recorder's to build (that note's *What S5 asks of the recorder*):

1. the level's minimap in the header;
2. a summary in the end block — each player's result, each commander death and defeat with its
   GameTime;
3. the mod: its name, its version, and a fingerprint of the archives the engine mounted;
4. a recording that starts mid-game from a snapshot, and an end reason for one the player stopped.

Asks 1 to 3 are *What the Replays browser asks of a recording* above. The fourth
remains to be built for S5's switches; live rewind operates on an existing recording.

## Open

- **Where recordings live and what they are called** (`replays\`, `.imprec`): chosen for S1, and
  shown by the browser in S5.
- **The stream's quantization and the crash promise** are the product page's open decisions 2 and
  5; the landings above measure what each needs. The snapshot interval, its decision 3, is decided
  (1,800 iterations, 2026-10-10).
- **`s_ghost`, the build ghost's piece masks,** is declared `SIM_STATE`, so it is recorded, though
  it is derived from the unit types and their scripts and only the draw reads it.
