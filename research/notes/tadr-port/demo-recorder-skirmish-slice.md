# Demo recorder — the skirmish slice (the plan)

## Status

The [product page](demo-recorder.md#milestones-and-delivery-order)'s second milestone, planned
2026-09-29: recording, puppet watching, seeking, live rewind and takeover, forks, the Replays browser
and `tacli` automation, with the verification the
[completeness design](demo-recorder-completeness.md#build-order) lists as its step 2. Six landings,
each a finished unit verified by running it. **S1, the recorder, landed on 2026-09-29**: a
skirmish is recorded to a file when `tagpu_recorder.on` asks for it, and `tools/replay/tarec.py`
reads a recording back. **S2, watching, is planned** (2026-09-30) as two landings, S2a and S2b;
S2a is next.

## How the slice is cut

Each landing adds one capability that can be verified by itself, and the ones after it build on it.

| Landing | Delivers | Verified by |
|---|---|---|
| **S1 — the recorder** | the capture at `0x4954ED`, the writer thread, the file, self-verification, and a tool that reads a recording back | self-verification over whole games; the file read back to the tick of a dump and compared with the dump byte for byte; the cost and the size measured |
| **S2a — what watching needs, recorded** | the objects a unit record points at (the mover, the order lists) and PlayerInfo, events at their roots, the nano spray, the header's game options, seats and weapon fingerprint | S1's checks over the new records; the events checked against the images |
| **S2b — watching** | the puppet host, playback, the one-writer rule, getter 75 per seat, `tacli replay play` | a recording played on puppets and compared with the recording at checkpoints; root counts equal and sink counts within a band |
| **S3 — seeking** | the rebuild in place from a snapshot, features and wrecks, the main block and `.data`, the replay harness and its restore round trip, the CLAUDE.md replay gate and the landing command's Step 4b, the nightly tier | the round trip, with its rules against a vacuous pass; seek times against the target |
| **S4 — rewind, takeover and forks** | the timeline in a live skirmish, Back to now, Play from here, forks in the file, the takeover from a recording | the harness's takeover run, judged like the compatibility suite's battle; the fork rules |
| **S5 — the browser and the controls** | the Replays browser, the timeline bar, perspectives, camera follow, the overlays, the recording switch in the settings screen | the browser and the controls driven through `tacli ui`, with pictures |

**S1 and S2 record every value exactly, at every tick.** That is what makes their checks exact. The
product page's stream — ten samples a second in whole world units, interpolated in playback — is a
landing of its own after S2, once playback can show its error; S1 measures the exact stream's size
so that the gap to the budget is known before then. Until that landing a skirmish is recorded only
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
- `tools/replay/tarec.py`: reads a recording — `info` (the header, the blocks and how it ended),
  `check` (the stream against every snapshot the file carries) and `compare` (the stream to a
  dump's GameTime against the dump, byte for byte).
- The verb `dump <name>` in `tagpu_replay.trigger`, looked for every 32 captures: the capture that
  finds it first writes the live world's images to `replays\dump-<name>.bin`, a stored snapshot of
  the state that capture records.

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
  `freed` and `local`, one entry per unit slot, whose row alone is recorded).
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

Planned 2026-09-30 from four research tracks: the mover and carried units, the order lists, the
events, and the puppet host. Their evidence lands with S2a. S2 is two landings. **S2a** records
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

  Estimated at +10 to +13 KB a battle iteration on the stream's 20 KB, not measured. Most of it is
  the moved-at stamp `+0x2A`, which holds GameTime on every tick a unit moves, then the speed and the
  velocity.
- **The order lists.** Each unit's main list `+0x5C` and sub list `+0x60`, recorded as value images
  and delta-coded against the recorder's own previous image. A node image holds the action, state,
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
    while recording; and **interceptions**.
  - **Script starts** through the three broadcasters `0x456190`, `0x456200` and `0x456290`.
  - **Build done**, `0x41B8D0`, the only writer of the model's nanoframe flag.
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

### S2b — watching

- **Opening a recording** goes through the engine's own SKIRMISH Start.
  `tacli replay play <instance> <recording>` writes the instance's skirmish settings from the
  header, copies the recording into `replays\`, writes the request file `tagpu_replay.play`, and
  clicks Start. Four sites are installed with the others, inert unless a request armed the level:
  - **The seat fill's call `0x47B092`.** It validates the request before any write: the format, the
    layout's hash, a skirmish, no network, the map's settings name, the seats and the unit
    definitions. Then it seats each recorded player as type 3 through `0x464290`, restores their
    PlayerInfo and alliance rows, seats the viewer, and disarms the victory and defeat tests
    (`0x4904B0`). On a failure the log names the reason and a normal skirmish starts.
  - **The settings persist's call `0x47B09C`.** It applies the recording's game options after
    `0x430F00` has written the player's own to the registry. The teardown restores them.
  - **The start units' calls `0x497963` and `0x497ACD`**, skipped.
  - **The applier's step at `0x4954ED`**, B2's stub. It checks what needs the loaded level, then
    applies.
- **The viewer's seat** is the lowest record the recording does not seat: type 3, with the watcher
  bit, holding `+0x2A42` for the whole level.
  - Every path that selects or orders walks the controlled player's units, of which this seat has
    none, so the viewer cannot select or order a puppet unit.
  - Four gates cover the rest: typed commands (`0x417B50`, every one refused), the debug keys
    (`0x4965E1`), the resource transfer (`0x464C60`) and saving (`0x4326B0`).
  - A recording that seats all ten records leaves no seat for the viewer, and is refused by name.
  - Full visibility fills the viewer's own sight. A perspective sets `+0x2A43` to a recorded seat.
- **The applier** runs each step on the game thread: image T's records, then E_T's events in
  recorded order. Every value is bounded before it becomes an index or a pointer. The completeness
  note's *What restoring these records needs* is its checklist.
  - **Units are placed with the engine's set-position `0x48A9F0`** at the recorded position. Then
    the walk-class and occupancy parts `0x43DA70` and `0x43DB50` run, which start the walk and
    occupy scripts. `+0x110` bits 2–3 and `+0x10A` are written only by those parts, and compared.
    The placement part `0x43D6D0` and the altitude fix-up `0x48A870` never run on a puppet: they
    would add the recorded velocity to the recorded position a second time, and fit the recorded
    altitude to the terrain.
  - **Carry links** follow the recorded chains through the engine's attach `0x48AB70`, bounded
    first. They are never written raw.
  - **Order lists** are reconciled against their images through the engine's constructor,
    reference set, linker and destructor.
  - **Events are replayed through the functions that made them:**
    - a death through `0x4866D0` in the receiver's mode, after the victim's record is written;
    - a create through `0x485F50`, into the recorded slot;
    - a shot through the receiver `0x49D270`;
    - a script start through `0x4B0B00`, with at most four arguments;
    - a reply through `0x47F780`, whose own viewer test plays the watched seat's.
  - **A recorded YARD_OPEN** is applied after the positions, through the re-claim `0x47C790` and
    the passability refresh `0x440A40`.
- **The recording is the only writer.** On a puppet host:
  - damage the host computes for a puppet is refused at the hit `0x489CE0`, at B3's existing drop;
  - the reaper `0x48AFD1` skips puppets;
  - the host's own meteor shower does not run;
  - the engine's own reply calls are inert;
  - Impure's per-slot hooks (B4's stamps, C4's marks) skip puppet slots;
  - every order node on a puppet is held disarmed (`puppet0`, `puppet1`).
- **Getter 75** answers, for each puppet seat, the recording machine's bit. The answer comes from
  a per-seat table rebuilt from the header at the seat fill, and declared `SIM_CONTENT` in a new
  `sim` module. The recorder refuses to record a puppet host.
- **Verified by:** a recorded skirmish played on puppets under Wine and compared with the recording
  at checkpoints: every recorded value identical but the approved rows, the root counts equal, and
  the sink counts within a band. It is played once in full visibility, and once from the recording
  viewer's perspective, where the radar picture is compared too.

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

### What S2 leaves

- **Selecting a puppet unit**, its order markers and a factory's queue: S5's perspectives and
  overlays.
- **Features, wrecks and the footprint grid's contested cells**: S3. Until then a replay's features
  change only by the replayed deaths' wrecks [INFERRED: a host applies no damage and runs no order
  handler, so its trees do not burn or fall and nothing is reclaimed].
- **The path a mover's class object holds.** A takeover plans a new one from the orders (S4).
- **Recordings that seat all ten records.**

## S3 to S5

Each is planned in detail when the landing before it is in. The table above is their scope; the
[completeness note](demo-recorder-completeness.md)'s [open items](demo-recorder-completeness.md#open-items)
are theirs.

## Open

- **Where recordings live and what they are called** (`replays\`, `.imprec`): chosen for S1, and
  shown by the browser in S5.
- **The stream's quantization, the snapshot interval and the crash promise** are the product page's
  open decisions 2, 3 and 5; the landings above measure what each needs.
- **`s_ghost`, the build ghost's piece masks,** is declared `SIM_STATE`, so it is recorded, though
  it is derived from the unit types and their scripts and only the draw reads it.
