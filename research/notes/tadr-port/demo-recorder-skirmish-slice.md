# Demo recorder — the skirmish slice (the plan)

## Status

The [product page](demo-recorder.md#milestones-and-delivery-order)'s second milestone, planned
2026-09-29: recording, puppet watching, seeking, live rewind and takeover, forks, the Replays browser
and `tacli` automation, with the verification the
[completeness design](demo-recorder-completeness.md#build-order) lists as its step 2. Five landings,
each a finished unit verified by running it. **S1, the recorder, landed on 2026-09-29**: every
skirmish is recorded to a file, and `tools/replay/tarec.py` reads a recording back. S2 is next.

## How the slice is cut

Each landing adds one capability that can be verified by itself, and the ones after it build on it.

| Landing | Delivers | Verified by |
|---|---|---|
| **S1 — the recorder** | the capture at `0x4954ED`, the writer thread, the file, self-verification, and a tool that reads a recording back | self-verification over whole games; the file read back to the tick of a dump and compared with the dump byte for byte; the cost and the size measured |
| **S2 — watching** | the objects a unit record points at (the mover, the order nodes), events at their roots, the puppet host, playback, the one-writer rule, getter 75 per seat, the nano spray | S1's checks over the new records; a recording played on puppets and compared with the recording at checkpoints; root counts equal and sink counts within a band |
| **S3 — seeking** | the rebuild in place from a snapshot, features and wrecks, the main block and `.data`, the replay harness and its restore round trip, the CLAUDE.md replay gate and the landing command's Step 4b, the nightly tier | the round trip, with its rules against a vacuous pass; seek times against the target |
| **S4 — rewind, takeover and forks** | the timeline in a live skirmish, Back to now, Play from here, forks in the file, the takeover from a recording | the harness's takeover run, judged like the compatibility suite's battle; the fork rules |
| **S5 — the browser and the controls** | the Replays browser, the timeline bar, perspectives, camera follow, the overlays, the recording switch in the settings screen | the browser and the controls driven through `tacli ui`, with pictures |

**S1 and S2 record every value exactly, at every tick.** That is what makes their checks exact. The
product page's stream — ten samples a second in whole world units, interpolated in playback — is a
landing of its own after S2, once playback can show its error; S1 measures the exact stream's size
so that the gap to the budget is known before then.

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
holds its target's ID in place of the pointer, by the table's offset rule; `NULL`, a pointer outside
its table and the off-map sort bucket get IDs of their own (`REC_ID_NULL`, `REC_ID_BAD`,
`REC_ID_OFFMAP`), the second counted and its first eight logged. The bytes of `object`, `cache`,
`timer` and `drop` rows are zero. A declared table's image follows its layout the same way: an
`SIMF_ADDR` field with an ID table holds the ID, and an address with none, an `SIMF_LOCAL` value and
the `SIMF_OWNED` and `SIMF_COLUMN` pointers are zero.

### The capture

`tagpu_recorder.c` runs first in B2's stub at the sim step's call of the unit tick, `0x4954ED`,
which then rebuilds the air pool and jumps to the tick. GameTime has just been incremented
(`0x4954C0`) and the network pumped (`0x4954C8`), and every phase of the previous iteration has run,
so the capture sees the world between two iterations, on the thread that owns it. It names engine
memory only to read it, each read bounded by the engine's own count
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
  when a table's count takes rows in; a row of zeros takes the zero row's image, computed once at
  the start.

The largest declared tables are heap tables for this reason: the build ghost's memo `s_ghost`
(16,384 rows of 52 bytes) and the transported explosions' marks `s_txMark` and `s_txKillMark`
(65,536 entries each). As static arrays their 1.3 MB was compared every iteration. Each is allocated
when its module installs and never freed, and every path that can run without that module tests it
for NULL. A capture that imaged every row of every table each iteration cost 3.6 ms; write-watched
heap tables, the byte comparisons and the zero row bring it to about 80 µs in a four-player game.

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
32-byte header — the block's kind, the first and last GameTime it covers, its raw and stored sizes, a
CRC-32 of the stored bytes and one of the header — and the payload, deflated with lodepng's
`lodepng_zlib_compress` when that is smaller, else stored. `inc/tagpu_recfmt.h` is the format:

- **the header block**: the format version, Impure's build, the game mode, the map, the first
  GameTime, the wall clock, the unit array's slots and the units each player may have, the unit
  types' count and a hash of their names, the players (index, controller, side, logo and name), the
  registry's levers and their text, the exclusion file's SHA-256, and the recorded declarations
  (index, class, key, row size, flags and name), so a reader can walk their images;
- **stream blocks** of whole iterations, each ending at the first iteration past 256 KB or at 300
  iterations;
- **a snapshot block** every 900 iterations (30 seconds at normal speed), the writer's whole copy.
  The interval is a placeholder: S3 chooses it by measurement against the seek target;
- **the end block**: why the recording ended, the last GameTime, the iterations, the corrections,
  the id rows found outside their tables, and the most the ring held.

A block is written whole or not at all, so a file cut short reads up to its last complete block, and
a file with no end block is incomplete.

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

A recording starts at the first capture of a skirmish level, and only when its end is certain:
`tagpu_reclaim`'s wrap of the teardown `0x491B60` tells the recorder on the game thread, on entry,
before the cascade frees the unit array (`0x485980`, called at `0x491B95`), and without the wrap the
recorder does not start. The end record is the capture's last access to the ring; the writer then
finishes the file, frees the ring and exits. Each recording owns its ring and its writer, so a level
that starts while the previous file is still being finished takes a new pair.

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
- `tagpu_simreg`: a walk of the declarations and of the levers for the recorder, and
  `MEM_WRITE_WATCH` on every heap table `SIM_ALLOC` takes.
- `tools/replay/tarec.py`: reads a recording — `info` (the header, the blocks and how it ended),
  `check` (the stream against every snapshot the file carries) and `compare` (the stream to a
  dump's GameTime against the dump, byte for byte).
- The verb `dump <name>` in `tagpu_replay.trigger`, looked for every 32 captures: the capture that
  finds it first writes the live world's images to `replays\dump-<name>.bin`, a stored snapshot of
  the state that capture records.

S1 has no switch: every skirmish is recorded. The product's opt-out is a setting, and the settings
store's only writer is its menu (`tagpu_settings.h`), so the setting comes with its menu row in S5.

### Verified by

Scripted games on Wine, on a private X display, in the raised build unless stated; each recording
read back with `tarec info`, `check` and, where the run took dumps, `compare`:

| run | the game | what it showed |
|---|---|---|
| 1 | three stock AIs and an idle human seat on Core Prime Industrial Area, eight minutes, three dumps | 14,610 iterations and no correction; every dump equal (26, 39 and 64 live units), and all 16 snapshots equal to the stream |
| 2 | the experiments' battle on Town & Country, 1,428 units applied, two minutes, three dumps; run twice | no correction and every dump equal, 1,171 to 1,407 live units at the dumps; the cost below |
| 3 | run 1's game saved a minute in and loaded from the in-game menu, then played a minute, two dumps | the recording in progress ended complete at the load, at GameTime 1846; the loaded game started paused, and once running recorded from GameTime 1847; both dumps equal, no correction |
| 4 | run 1's game, the process killed with SIGKILL two minutes in | the file said incomplete and read to GameTime 3601, its four snapshots equal to the stream; copies cut inside a payload and inside a block header, and one with a torn block header, read to the block before the damage |
| 5 | run 1's game in a scratch build, not landed, that flips one byte of the writer's copy of a unit, a player and a declared table at GameTime 1000 | each correction named in the log and in the end block: the player at GameTime 1001, the unit at 1018, `g_def` row 0 `+0x8` at 1032; the dumps equal |
| — | run 1's game for 90 s in the stock-limits build | 22 tables, every dump equal, no correction |

### Measured

The capture's cost per iteration, by `QueryPerformanceCounter` around it, on the reference setup
under Wine:

| game | the first iteration | the rest, on average | the rest, at worst |
|---|---|---|---|
| run 1, up to 64 live units | 4.3 ms | 78 µs | 760 µs |
| run 2, 1,171 to 1,407 live units | 4.5 and 4.8 ms | 251 and 235 µs | 583 and 610 µs |

The product page's budget is 0.5 ms on average and 1 ms at worst with 1,500 units. The average
meets it. The worst did not once: an earlier battle run of the same game measured one iteration of
2.3 ms, whose cause was not found, and a timing probe over a third run saw none above 0.7 ms. The
first iteration images every row of every table, the extra weapons' 16,384 unit-type rows of 140
bytes among them, and puts into the ring only the rows that are not zero; in the stock-limits build,
whose unit-type tables are 512 rows, it costs 0.6 ms.

Size, the exact stream with every value at every tick: run 1's file is 2.0 MB for 8.1 minutes,
14.8 MB an hour at that rate — its stream deflated 3.4:1 to 1.7 MB, and its 16 snapshots, mostly
zero rows, add 0.26 MB — against the budget's 25 MB for a four-player game, which a longer game with
more units would pass. The two-minute battles came to 18.5 and 19.8 MB a minute (the stream deflates
about 2:1 there), 1.1 to 1.2 GB an hour if sustained, against the budget's 100 MB for ten players
with 1,500 units. In every run the ring held at most 1.2 MB of its 16 MB.

### Gaps S1 leaves

- **The objects a record points at** — the mover, the order lists, the COB object, the model, the AI
  object, the control groups, the sight counters and the resource accumulator — are not recorded:
  their rows are `object` and zero in the image. S2 brings the mover and the order nodes, with layouts
  of their own: new records in `replay-layout.tsv`, so new rows for the owner.
- **Memory a declaration reaches through a pointer** is not recorded: the engine pools the DLL holds
  (`SIM_ENGINE`: explosions, particle systems and their auxiliary geometry), the blocks a row owns
  (`SIMF_OWNED`), and the columns of a struct-of-arrays table (`SIMF_COLUMN`: `s_hit`'s `stamp`,
  `freed` and `local`, one entry per unit slot, whose row alone is recorded).
- **The header** has no game options and no fingerprint of the weapon definitions. Alliances are
  values of the player records, so the stream carries them.
- **The worst iteration** met the 1 ms budget in two battle runs of three (*Measured*).
- **Events** (shots, script starts, sounds, the nano spray): S2.
- **Features, wrecks, the main block and `.data`**: S3.
- **Network games** are not recorded: the multiplayer slice.

## S2 to S5

Each is planned in detail when the landing before it is in. The table above is their scope; the
[completeness note](demo-recorder-completeness.md)'s *What restoring these records needs* is the
applier's checklist for S2 and S3, and its [open items](demo-recorder-completeness.md#open-items) are
theirs. Rules S2 inherits from the notes:

- the puppet host seats every recorded player as a remote seat, disarms the victory and defeat
  tests, stamps each puppet's `+0x18` every tick, and steps only the movement pass's three
  presentation parts, writing the mover's layer and its `+0x2E` bit 2
  ([state evidence](demo-recorder-state-evidence.md#recording-and-replay-measured));
- events are played through the engine's own receivers, with every index bounded first;
- on a puppet the recorder is the only writer of the state the recording carries.

## Open

- **Where recordings live and what they are called** (`replays\`, `.imprec`): chosen for S1, and
  shown by the browser in S5.
- **A release cut between S1 and S5** would write a recording of every skirmish, with no switch and
  nothing that can open it yet, at the exact stream's size (*Measured*).
- **The stream's quantization, the snapshot interval and the crash promise** are the product page's
  open decisions 2, 3 and 5; the landings above measure what each needs.
- **`s_ghost`, the build ghost's piece masks,** is declared `SIM_STATE`, so it is recorded, though
  it is derived from the unit types and their scripts and only the draw reads it.
